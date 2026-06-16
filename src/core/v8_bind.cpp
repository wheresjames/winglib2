#include "wl2/v8_bind.h"

#include "wl2/runtime.h"

#include <iostream>
#include <memory>
#include <string>

#if WL2_ENABLE_V8
#include <libplatform/libplatform.h>
#include <v8.h>
#endif

namespace wl2 {

#if WL2_ENABLE_V8
namespace {

class V8Platform {
public:
    V8Platform() {
        v8::V8::InitializeICUDefaultLocation("");
        v8::V8::InitializeExternalStartupData("");
        platform_ = v8::platform::NewDefaultPlatform();
        v8::V8::InitializePlatform(platform_.get());
        v8::V8::Initialize();
    }

    ~V8Platform() {
        v8::V8::Dispose();
        v8::V8::DisposePlatform();
    }

private:
    std::unique_ptr<v8::Platform> platform_;
};

void console_log(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* isolate = args.GetIsolate();
    v8::HandleScope scope(isolate);
    for (int i = 0; i < args.Length(); ++i) {
        v8::String::Utf8Value value(isolate, args[i]);
        if (i) {
            std::cout << ' ';
        }
        std::cout << (*value ? *value : "");
    }
    std::cout << std::endl;
}

void make_console(v8::Isolate* isolate, v8::Local<v8::Context> context) {
    auto console = v8::Object::New(isolate);
    console->Set(context,
        v8::String::NewFromUtf8Literal(isolate, "log"),
        v8::Function::New(context, console_log).ToLocalChecked()).Check();
    context->Global()->Set(context,
        v8::String::NewFromUtf8Literal(isolate, "console"),
        console).Check();
}

void make_wl2_object(Runtime& runtime, v8::Isolate* isolate, v8::Local<v8::Context> context) {
    auto wl2obj = v8::Object::New(isolate);
    auto resources = v8::Array::New(isolate);
    int index = 0;
    for (const auto& name : runtime.resources().names()) {
        resources->Set(context, index++, v8::String::NewFromUtf8(isolate, name.c_str()).ToLocalChecked()).Check();
    }
    wl2obj->Set(context, v8::String::NewFromUtf8Literal(isolate, "resources"), resources).Check();
    context->Global()->Set(context, v8::String::NewFromUtf8Literal(isolate, "wl2"), wl2obj).Check();
}

} // namespace
#endif

int run_v8_script(Runtime& runtime, std::string_view name, std::string_view source) {
#if WL2_ENABLE_V8
    static V8Platform platform;

    v8::Isolate::CreateParams createParams;
    createParams.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    auto* isolate = v8::Isolate::New(createParams);
    {
        v8::Isolate::Scope isolateScope(isolate);
        v8::HandleScope handleScope(isolate);
        auto context = v8::Context::New(isolate);
        v8::Context::Scope contextScope(context);

        make_console(isolate, context);
        make_wl2_object(runtime, isolate, context);

        v8::TryCatch tryCatch(isolate);
        auto sourceString = v8::String::NewFromUtf8(isolate, source.data(), v8::NewStringType::kNormal, static_cast<int>(source.size())).ToLocalChecked();
        auto nameString = v8::String::NewFromUtf8(isolate, name.data(), v8::NewStringType::kNormal, static_cast<int>(name.size())).ToLocalChecked();
        v8::ScriptOrigin origin(nameString);
        v8::ScriptCompiler::Source scriptSource(sourceString, origin);
        v8::Local<v8::Script> script;
        if (!v8::ScriptCompiler::Compile(context, &scriptSource).ToLocal(&script)) {
            v8::String::Utf8Value error(isolate, tryCatch.Exception());
            std::cerr << (*error ? *error : "JavaScript compile error") << std::endl;
            isolate->Dispose();
            delete createParams.array_buffer_allocator;
            return 1;
        }
        v8::Local<v8::Value> result;
        if (!script->Run(context).ToLocal(&result)) {
            v8::String::Utf8Value error(isolate, tryCatch.Exception());
            std::cerr << (*error ? *error : "JavaScript runtime error") << std::endl;
            isolate->Dispose();
            delete createParams.array_buffer_allocator;
            return 1;
        }
    }
    isolate->Dispose();
    delete createParams.array_buffer_allocator;
    return 0;
#else
    (void)runtime;
    (void)name;
    std::cout << source;
    return 0;
#endif
}

} // namespace wl2
