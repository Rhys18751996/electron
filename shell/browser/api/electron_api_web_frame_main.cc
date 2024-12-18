// Copyright (c) 2020 Samuel Maddock <sam@samuelmaddock.com>.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/api/electron_api_web_frame_main.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/feature_list.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"  // nogncheck
#include "content/browser/renderer_host/render_process_host_impl.h"  // nogncheck
#include "content/public/browser/frame_tree_node_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/common/isolated_world_ids.h"
#include "electron/shell/common/api/api.mojom.h"
#include "gin/handle.h"
#include "gin/object_template_builder.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "shell/browser/api/message_port.h"
#include "shell/browser/browser.h"
#include "shell/browser/javascript_environment.h"
#include "shell/common/gin_converters/blink_converter.h"
#include "shell/common/gin_converters/frame_converter.h"
#include "shell/common/gin_converters/gurl_converter.h"
#include "shell/common/gin_converters/value_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/error_thrower.h"
#include "shell/common/gin_helper/object_template_builder.h"
#include "shell/common/gin_helper/promise.h"
#include "shell/common/node_includes.h"
#include "shell/common/v8_util.h"

namespace {

// RenderFrameHost (RFH) exists as a child of a FrameTreeNode. When a
// cross-origin navigation occurs, the FrameTreeNode swaps RFHs. After
// swapping, the old RFH will be marked for deletion and run any unload
// listeners. If an IPC is sent during an unload/beforeunload listener,
// it's possible that it arrives after the RFH swap and has been
// detached from the FrameTreeNode.
bool IsDetachedFrameHost(content::RenderFrameHost* rfh) {
  if (!rfh)
    return true;

  // RenderFrameCreated is called for speculative frames which may not be
  // used in certain cross-origin navigations. Invoking
  // RenderFrameHost::GetLifecycleState currently crashes when called for
  // speculative frames so we need to filter it out for now. Check
  // https://crbug.com/1183639 for details on when this can be removed.
  auto* rfh_impl = static_cast<content::RenderFrameHostImpl*>(rfh);

  // During cross-origin navigation, a RFH may be swapped out of its
  // FrameTreeNode with a new RFH. In these cases, it's marked for
  // deletion. As this pending deletion RFH won't be following future
  // swaps, we need to indicate that its been pinned.
  return (rfh_impl->lifecycle_state() !=
              content::RenderFrameHostImpl::LifecycleStateImpl::kSpeculative &&
          rfh->GetLifecycleState() ==
              content::RenderFrameHost::LifecycleState::kPendingDeletion);
}

}  // namespace

namespace gin {

template <>
struct Converter<blink::mojom::PageVisibilityState> {
  static v8::Local<v8::Value> ToV8(v8::Isolate* isolate,
                                   blink::mojom::PageVisibilityState val) {
    std::string visibility;
    switch (val) {
      case blink::mojom::PageVisibilityState::kVisible:
        visibility = "visible";
        break;
      case blink::mojom::PageVisibilityState::kHidden:
      case blink::mojom::PageVisibilityState::kHiddenButPainting:
        visibility = "hidden";
        break;
    }
    return gin::ConvertToV8(isolate, visibility);
  }
};

}  // namespace gin

namespace electron::api {

// FrameTreeNodeId -> WebFrameMain*
// Using FrameTreeNode allows us to track frame across navigations. This
// is most similar to how <iframe> works.
using FrameTreeNodeIdMap = std::unordered_map<content::FrameTreeNodeId,
                                              WebFrameMain*,
                                              content::FrameTreeNodeId::Hasher>;

// Token -> WebFrameMain*
// Maps exact RFH to a WebFrameMain instance.
using FrameTokenMap =
    std::map<content::GlobalRenderFrameHostToken, WebFrameMain*>;

namespace {

FrameTreeNodeIdMap& GetFrameTreeNodeIdMap() {
  static base::NoDestructor<FrameTreeNodeIdMap> instance;
  return *instance;
}
FrameTokenMap& GetFrameTokenMap() {
  static base::NoDestructor<FrameTokenMap> instance;
  return *instance;
}

}  // namespace

// static
WebFrameMain* WebFrameMain::FromFrameTreeNodeId(
    content::FrameTreeNodeId frame_tree_node_id) {
  // Pinned frames aren't tracked across navigations so only non-pinned
  // frames will be retrieved.
  FrameTreeNodeIdMap& frame_map = GetFrameTreeNodeIdMap();
  auto iter = frame_map.find(frame_tree_node_id);
  auto* web_frame = iter == frame_map.end() ? nullptr : iter->second;
  return web_frame;
}

// static
WebFrameMain* WebFrameMain::FromFrameToken(
    content::GlobalRenderFrameHostToken frame_token) {
  FrameTokenMap& frame_map = GetFrameTokenMap();
  auto iter = frame_map.find(frame_token);
  auto* web_frame = iter == frame_map.end() ? nullptr : iter->second;
  return web_frame;
}

// static
WebFrameMain* WebFrameMain::FromRenderFrameHost(content::RenderFrameHost* rfh) {
  if (!rfh)
    return nullptr;
  return FromFrameToken(rfh->GetGlobalFrameToken());
}

gin::WrapperInfo WebFrameMain::kWrapperInfo = {gin::kEmbedderNativeGin};

WebFrameMain::WebFrameMain(content::RenderFrameHost* rfh)
    : frame_tree_node_id_(rfh->GetFrameTreeNodeId()),
      frame_token_(rfh->GetGlobalFrameToken()),
      render_frame_(rfh),
      render_frame_detached_(IsDetachedFrameHost(rfh)) {
  GetFrameTreeNodeIdMap().emplace(frame_tree_node_id_, this);
  GetFrameTokenMap().emplace(frame_token_, this);
}

WebFrameMain::~WebFrameMain() {
  Destroyed();
}

void WebFrameMain::Destroyed() {
  MarkRenderFrameDisposed();
  GetFrameTreeNodeIdMap().erase(frame_tree_node_id_);
  GetFrameTokenMap().erase(frame_token_);
  Unpin();
}

void WebFrameMain::MarkRenderFrameDisposed() {
  render_frame_ = nullptr;
  render_frame_detached_ = true;
  render_frame_disposed_ = true;
  TeardownMojoConnection();
}

// Should only be called when swapping frames.
void WebFrameMain::UpdateRenderFrameHost(content::RenderFrameHost* rfh) {
  GetFrameTokenMap().erase(frame_token_);
  frame_token_ = rfh->GetGlobalFrameToken();
  GetFrameTokenMap().emplace(frame_token_, this);

  render_frame_disposed_ = false;
  render_frame_ = rfh;
  TeardownMojoConnection();
  MaybeSetupMojoConnection();
}

bool WebFrameMain::CheckRenderFrame() const {
  if (render_frame_disposed_) {
    v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
    v8::HandleScope scope(isolate);
    gin_helper::ErrorThrower(isolate).ThrowError(
        "Render frame was disposed before WebFrameMain could be accessed");
    return false;
  }
  return true;
}

v8::Local<v8::Promise> WebFrameMain::ExecuteJavaScript(
    gin::Arguments* args,
    const std::u16string& code) {
  gin_helper::Promise<base::Value> promise(args->isolate());
  v8::Local<v8::Promise> handle = promise.GetHandle();

  // Optional userGesture parameter
  bool user_gesture;
  if (!args->PeekNext().IsEmpty()) {
    if (args->PeekNext()->IsBoolean()) {
      args->GetNext(&user_gesture);
    } else {
      args->ThrowTypeError("userGesture must be a boolean");
      return handle;
    }
  } else {
    user_gesture = false;
  }

  if (render_frame_disposed_) {
    promise.RejectWithErrorMessage(
        "Render frame was disposed before WebFrameMain could be accessed");
    return handle;
  }

  static_cast<content::RenderFrameHostImpl*>(render_frame_)
      ->ExecuteJavaScriptForTests(
          code, user_gesture, true /* resolve_promises */,
          /*honor_js_content_settings=*/true, content::ISOLATED_WORLD_ID_GLOBAL,
          base::BindOnce(
              [](gin_helper::Promise<base::Value> promise,
                 blink::mojom::JavaScriptExecutionResultType type,
                 base::Value value) {
                if (type ==
                    blink::mojom::JavaScriptExecutionResultType::kSuccess) {
                  promise.Resolve(value);
                } else {
                  v8::Isolate* isolate = JavascriptEnvironment::GetIsolate();
                  v8::HandleScope scope(isolate);
                  promise.Reject(gin::ConvertToV8(isolate, value));
                }
              },
              std::move(promise)));

  return handle;
}

bool WebFrameMain::Reload() {
  if (!CheckRenderFrame())
    return false;
  return render_frame_->Reload();
}

bool WebFrameMain::IsDestroyed() const {
  return render_frame_disposed_;
}

void WebFrameMain::Send(v8::Isolate* isolate,
                        bool internal,
                        const std::string& channel,
                        v8::Local<v8::Value> args) {
  blink::CloneableMessage message;
  if (!gin::ConvertFromV8(isolate, args, &message)) {
    isolate->ThrowException(v8::Exception::Error(
        gin::StringToV8(isolate, "Failed to serialize arguments")));
    return;
  }

  if (!CheckRenderFrame())
    return;

  GetRendererApi()->Message(internal, channel, std::move(message));
}

const mojo::Remote<mojom::ElectronRenderer>& WebFrameMain::GetRendererApi() {
  MaybeSetupMojoConnection();
  return renderer_api_;
}

void WebFrameMain::MaybeSetupMojoConnection() {
  if (render_frame_disposed_) {
    // RFH may not be set yet if called between when a new RFH is created and
    // before it's been swapped with an old RFH.
    LOG(INFO) << "Attempt to setup WebFrameMain connection while render frame "
                 "is disposed";
    return;
  }

  if (!renderer_api_) {
    pending_receiver_ = renderer_api_.BindNewPipeAndPassReceiver();
    renderer_api_.set_disconnect_handler(base::BindOnce(
        &WebFrameMain::OnRendererConnectionError, weak_factory_.GetWeakPtr()));
  }

  DCHECK(render_frame_);

  // Wait for RenderFrame to be created in renderer before accessing remote.
  if (pending_receiver_ && render_frame_ &&
      render_frame_->IsRenderFrameLive()) {
    render_frame_->GetRemoteInterfaces()->GetInterface(
        std::move(pending_receiver_));
  }
}

void WebFrameMain::TeardownMojoConnection() {
  renderer_api_.reset();
  pending_receiver_.reset();
}

void WebFrameMain::OnRendererConnectionError() {
  TeardownMojoConnection();
}

void WebFrameMain::PostMessage(v8::Isolate* isolate,
                               const std::string& channel,
                               v8::Local<v8::Value> message_value,
                               std::optional<v8::Local<v8::Value>> transfer) {
  blink::TransferableMessage transferable_message;
  if (!electron::SerializeV8Value(isolate, message_value,
                                  &transferable_message)) {
    // SerializeV8Value sets an exception.
    return;
  }

  std::vector<gin::Handle<MessagePort>> wrapped_ports;
  if (transfer && !transfer.value()->IsUndefined()) {
    if (!gin::ConvertFromV8(isolate, *transfer, &wrapped_ports)) {
      isolate->ThrowException(v8::Exception::Error(
          gin::StringToV8(isolate, "Invalid value for transfer")));
      return;
    }
  }

  bool threw_exception = false;
  transferable_message.ports =
      MessagePort::DisentanglePorts(isolate, wrapped_ports, &threw_exception);
  if (threw_exception)
    return;

  if (!CheckRenderFrame())
    return;

  GetRendererApi()->ReceivePostMessage(channel,
                                       std::move(transferable_message));
}

bool WebFrameMain::Detached() const {
  return render_frame_detached_;
}

content::FrameTreeNodeId WebFrameMain::FrameTreeNodeID() const {
  return frame_tree_node_id_;
}

std::string WebFrameMain::Name() const {
  if (!CheckRenderFrame())
    return {};
  return render_frame_->GetFrameName();
}

base::ProcessId WebFrameMain::OSProcessID() const {
  if (!CheckRenderFrame())
    return -1;
  base::ProcessHandle process_handle =
      render_frame_->GetProcess()->GetProcess().Handle();
  return base::GetProcId(process_handle);
}

int WebFrameMain::ProcessID() const {
  if (!CheckRenderFrame())
    return -1;
  return render_frame_->GetProcess()->GetDeprecatedID();
}

int WebFrameMain::RoutingID() const {
  if (!CheckRenderFrame())
    return -1;
  return render_frame_->GetRoutingID();
}

GURL WebFrameMain::URL() const {
  if (!CheckRenderFrame())
    return GURL::EmptyGURL();
  return render_frame_->GetLastCommittedURL();
}

std::string WebFrameMain::Origin() const {
  if (!CheckRenderFrame())
    return {};
  return render_frame_->GetLastCommittedOrigin().Serialize();
}

blink::mojom::PageVisibilityState WebFrameMain::VisibilityState() const {
  if (!CheckRenderFrame())
    return blink::mojom::PageVisibilityState::kHidden;
  return render_frame_->GetVisibilityState();
}

content::RenderFrameHost* WebFrameMain::Top() const {
  if (!CheckRenderFrame())
    return nullptr;
  return render_frame_->GetMainFrame();
}

content::RenderFrameHost* WebFrameMain::Parent() const {
  if (!CheckRenderFrame())
    return nullptr;
  return render_frame_->GetParent();
}

std::vector<content::RenderFrameHost*> WebFrameMain::Frames() const {
  std::vector<content::RenderFrameHost*> frame_hosts;
  if (!CheckRenderFrame())
    return frame_hosts;

  render_frame_->ForEachRenderFrameHost(
      [&frame_hosts, this](content::RenderFrameHost* rfh) {
        if (rfh->GetParent() == render_frame_)
          frame_hosts.push_back(rfh);
      });

  return frame_hosts;
}

std::vector<content::RenderFrameHost*> WebFrameMain::FramesInSubtree() const {
  std::vector<content::RenderFrameHost*> frame_hosts;
  if (!CheckRenderFrame())
    return frame_hosts;

  render_frame_->ForEachRenderFrameHost(
      [&frame_hosts](content::RenderFrameHost* rfh) {
        frame_hosts.push_back(rfh);
      });

  return frame_hosts;
}

v8::Local<v8::Promise> WebFrameMain::CollectDocumentJSCallStack(
    gin::Arguments* args) {
  gin_helper::Promise<base::Value> promise(args->isolate());
  v8::Local<v8::Promise> handle = promise.GetHandle();

  if (render_frame_disposed_) {
    promise.RejectWithErrorMessage(
        "Render frame was disposed before WebFrameMain could be accessed");
    return handle;
  }

  if (!base::FeatureList::IsEnabled(
          blink::features::kDocumentPolicyIncludeJSCallStacksInCrashReports)) {
    promise.RejectWithErrorMessage(
        "DocumentPolicyIncludeJSCallStacksInCrashReports is not enabled");
    return handle;
  }

  content::RenderProcessHostImpl* rph_impl =
      static_cast<content::RenderProcessHostImpl*>(render_frame_->GetProcess());

  rph_impl->GetJavaScriptCallStackGeneratorInterface()
      ->CollectJavaScriptCallStack(
          base::BindOnce(&WebFrameMain::CollectedJavaScriptCallStack,
                         weak_factory_.GetWeakPtr(), std::move(promise)));

  return handle;
}

void WebFrameMain::CollectedJavaScriptCallStack(
    gin_helper::Promise<base::Value> promise,
    const std::string& untrusted_javascript_call_stack,
    const std::optional<blink::LocalFrameToken>& remote_frame_token) {
  if (render_frame_disposed_) {
    promise.RejectWithErrorMessage(
        "Render frame was disposed before call stack was received");
    return;
  }

  const blink::LocalFrameToken& frame_token = render_frame_->GetFrameToken();
  if (remote_frame_token == frame_token) {
    base::Value base_value(untrusted_javascript_call_stack);
    promise.Resolve(base_value);
  } else if (!remote_frame_token) {
    // Failed to collect call stack. See logic in:
    // third_party/blink/renderer/controller/javascript_call_stack_collector.cc
    promise.Resolve(base::Value());
  } else {
    // Requests for call stacks can be initiated on an old RenderProcessHost
    // then be received after a frame swap.
    LOG(ERROR) << "Received call stack from old RPH";
    promise.Resolve(base::Value());
  }
}

void WebFrameMain::DOMContentLoaded() {
  Emit("dom-ready");
}

// static
gin::Handle<WebFrameMain> WebFrameMain::New(v8::Isolate* isolate) {
  return {};
}

// static
gin::Handle<WebFrameMain> WebFrameMain::From(v8::Isolate* isolate,
                                             content::RenderFrameHost* rfh) {
  if (!rfh)
    return {};

  auto* web_frame = FromRenderFrameHost(rfh);
  if (web_frame)
    return gin::CreateHandle(isolate, web_frame);

  auto handle = gin::CreateHandle(isolate, new WebFrameMain(rfh));

  // Prevent garbage collection of frame until it has been deleted internally.
  handle->Pin(isolate);

  return handle;
}

// static
void WebFrameMain::FillObjectTemplate(v8::Isolate* isolate,
                                      v8::Local<v8::ObjectTemplate> templ) {
  gin_helper::ObjectTemplateBuilder(isolate, templ)
      .SetMethod("executeJavaScript", &WebFrameMain::ExecuteJavaScript)
      .SetMethod("collectJavaScriptCallStack",
                 &WebFrameMain::CollectDocumentJSCallStack)
      .SetMethod("reload", &WebFrameMain::Reload)
      .SetMethod("isDestroyed", &WebFrameMain::IsDestroyed)
      .SetMethod("_send", &WebFrameMain::Send)
      .SetMethod("_postMessage", &WebFrameMain::PostMessage)
      .SetProperty("detached", &WebFrameMain::Detached)
      .SetProperty("frameTreeNodeId", &WebFrameMain::FrameTreeNodeID)
      .SetProperty("name", &WebFrameMain::Name)
      .SetProperty("osProcessId", &WebFrameMain::OSProcessID)
      .SetProperty("processId", &WebFrameMain::ProcessID)
      .SetProperty("routingId", &WebFrameMain::RoutingID)
      .SetProperty("url", &WebFrameMain::URL)
      .SetProperty("origin", &WebFrameMain::Origin)
      .SetProperty("visibilityState", &WebFrameMain::VisibilityState)
      .SetProperty("top", &WebFrameMain::Top)
      .SetProperty("parent", &WebFrameMain::Parent)
      .SetProperty("frames", &WebFrameMain::Frames)
      .SetProperty("framesInSubtree", &WebFrameMain::FramesInSubtree)
      .Build();
}

const char* WebFrameMain::GetTypeName() {
  return GetClassName();
}

}  // namespace electron::api

namespace {

using electron::api::WebFrameMain;

v8::Local<v8::Value> FromID(gin_helper::ErrorThrower thrower,
                            int render_process_id,
                            int render_frame_id) {
  if (!electron::Browser::Get()->is_ready()) {
    thrower.ThrowError("WebFrameMain is available only after app ready");
    return v8::Null(thrower.isolate());
  }

  auto* rfh =
      content::RenderFrameHost::FromID(render_process_id, render_frame_id);
  if (!rfh)
    return v8::Undefined(thrower.isolate());

  return WebFrameMain::From(thrower.isolate(), rfh).ToV8();
}

v8::Local<v8::Value> FromIdIfExists(gin_helper::ErrorThrower thrower,
                                    int render_process_id,
                                    int render_frame_id) {
  if (!electron::Browser::Get()->is_ready()) {
    thrower.ThrowError("WebFrameMain is available only after app ready");
    return v8::Null(thrower.isolate());
  }
  content::RenderFrameHost* rfh =
      content::RenderFrameHost::FromID(render_process_id, render_frame_id);
  WebFrameMain* web_frame = WebFrameMain::FromRenderFrameHost(rfh);
  if (!web_frame)
    return v8::Null(thrower.isolate());
  return gin::CreateHandle(thrower.isolate(), web_frame).ToV8();
}

v8::Local<v8::Value> FromFtnIdIfExists(gin_helper::ErrorThrower thrower,
                                       int frame_tree_node_id) {
  if (!electron::Browser::Get()->is_ready()) {
    thrower.ThrowError("WebFrameMain is available only after app ready");
    return v8::Null(thrower.isolate());
  }
  WebFrameMain* web_frame = WebFrameMain::FromFrameTreeNodeId(
      content::FrameTreeNodeId(frame_tree_node_id));
  if (!web_frame)
    return v8::Null(thrower.isolate());
  return gin::CreateHandle(thrower.isolate(), web_frame).ToV8();
}

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  v8::Isolate* isolate = context->GetIsolate();
  gin_helper::Dictionary dict(isolate, exports);
  dict.Set("WebFrameMain", WebFrameMain::GetConstructor(context));
  dict.SetMethod("fromId", &FromID);
  dict.SetMethod("_fromIdIfExists", &FromIdIfExists);
  dict.SetMethod("_fromFtnIdIfExists", &FromFtnIdIfExists);
}

}  // namespace

NODE_LINKED_BINDING_CONTEXT_AWARE(electron_browser_web_frame_main, Initialize)
