#include "pch.h"

#include "OnnxruntimeEngine.h"

#include "PheonixSingleton.h"
#include "OnnxruntimeEnvironment.h"
#include "OnnxruntimeEngineBuilder.h"
#include "OnnxruntimeModel.h"
#include "OnnxruntimeSessionBuilder.h"

#include "core/providers/winml/winml_provider_factory.h"

using namespace WinML;

static ONNXTensorElementDataType
ONNXTensorElementDataTypeFromTensorKind(winml::TensorKind kind) {
  switch (kind) {
    case winml::TensorKind::Boolean:    { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL;       }
    case winml::TensorKind::String:     { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING;     }
    case winml::TensorKind::Float16:    { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;    }
    case winml::TensorKind::Float:      { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;      }
    case winml::TensorKind::Double:     { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE;     }
    case winml::TensorKind::Int8:       { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8;       }
    case winml::TensorKind::Int16:      { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16;      }
    case winml::TensorKind::Int32:      { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;      }
    case winml::TensorKind::Int64:      { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;      }
    case winml::TensorKind::UInt8:      { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;      }
    case winml::TensorKind::UInt16:     { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16;     }
    case winml::TensorKind::UInt32:     { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32;     }
    case winml::TensorKind::UInt64:     { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64;     }
    case winml::TensorKind::Complex64:  { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX64;  }
    case winml::TensorKind::Complex128: { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX128; }
    default:                            { return ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;  }
  }
}

OnnxruntimeValue::OnnxruntimeValue() : value_(nullptr, nullptr), allocator_(nullptr, nullptr) {}

OnnxruntimeValue::~OnnxruntimeValue() {
  value_.release();
  allocator_.release();
}

HRESULT OnnxruntimeValue::RuntimeClassInitialize(OnnxruntimeEngineFactory* engine_factory, OnnxruntimeEngine* engine, UniqueOrtValue&& ort_value, UniqueOrtAllocator&& allocator) {
  engine_factory_ = engine_factory;
  engine_ = engine;
  value_ = std::move(ort_value);
  allocator_ = std::move(allocator);

  return S_OK;
}

HRESULT OnnxruntimeValue::IsCpu(bool* out) {
  auto ort_api = engine_factory_->UseOrtApi();
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();

  OrtMemoryInfo* ort_memory_info;
  winml_adapter_api->GetValueMemoryInfo(value_.get(), &ort_memory_info);
  auto memory_info = UniqueOrtMemoryInfo(ort_memory_info, ort_api->ReleaseMemoryInfo);

  const char* name;
  ort_api->MemoryInfoGetName(memory_info.get(), &name);

  OrtMemType type;
  ort_api->MemoryInfoGetMemType(memory_info.get(), &type);

  *out = !strcmp(name, "Cpu") ||
         type == OrtMemType::OrtMemTypeCPUOutput ||
         type == OrtMemType::OrtMemTypeCPUInput;
  return S_OK;
}

HRESULT OnnxruntimeValue::GetResource(void** resource) {
  auto ort_api = engine_factory_->UseOrtApi();
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();
  
  void* mutable_data = nullptr;
  ort_api->GetTensorMutableData(value_.get(), &mutable_data);

  OrtExecutionProvider* ort_provider;
  winml_adapter_api->SessionGetExecutionProvider(engine_->UseOrtSession(), 0, &ort_provider);

  if (engine_->IsDmlSession()) {
    winml_adapter_api->DmlGetD3D12ResourceFromAllocation(ort_provider, mutable_data,
        reinterpret_cast<ID3D12Resource**>(resource));
  } 
  else {
    *resource = mutable_data;
  }
  return S_OK;
}

HRESULT OnnxruntimeValue::IsTensor(bool* out) {
  auto ort_api = engine_factory_->UseOrtApi();

  ONNXType type = ONNXType::ONNX_TYPE_UNKNOWN;
  ort_api->GetValueType(value_.get(), &type);
  *out = type == ONNXType::ONNX_TYPE_TENSOR;
  return S_OK;
}

HRESULT OnnxruntimeValue::IsOfTensorType(winml::TensorKind kind, bool* out) {
  auto ort_api = engine_factory_->UseOrtApi();
  OrtTensorTypeAndShapeInfo* info = nullptr;
  ort_api->GetTensorTypeAndShape(value_.get(), &info);
  auto type_and_shape_info = UniqueOrtTensorTypeAndShapeInfo(info, ort_api->ReleaseTensorTypeAndShapeInfo);

  ONNXTensorElementDataType data_type = ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  ort_api->GetTensorElementType(type_and_shape_info.get(), &data_type);

  *out = data_type == ONNXTensorElementDataTypeFromTensorKind(kind);
  return S_OK;
}

HRESULT OnnxruntimeValue::GetTensorShape(std::vector<int64_t>& shape_vector) {
  auto ort_api = engine_factory_->UseOrtApi();
  OrtTensorTypeAndShapeInfo* info = nullptr;
  ort_api->GetTensorTypeAndShape(value_.get(), &info);
  auto type_and_shape_info = UniqueOrtTensorTypeAndShapeInfo(info, ort_api->ReleaseTensorTypeAndShapeInfo);

  size_t size;
  ort_api->GetDimensionsCount(type_and_shape_info.get(), &size);

  std::vector<int64_t> shape(size);
  ort_api->GetDimensions(type_and_shape_info.get(), &shape[0], size);

  shape_vector = std::move(shape);
  return S_OK;
}

HRESULT OnnxruntimeValue::IsOfMapType(winml::TensorKind key_kind, winml::TensorKind value_kind, bool* out) {
  return E_NOTIMPL;
}

HRESULT OnnxruntimeValue::IsOfVectorMapType(winml::TensorKind key_kind, winml::TensorKind value_kind, bool* out) {
  return E_NOTIMPL;
}

OnnxruntimeEngine::OnnxruntimeEngine() : session_(nullptr, nullptr) {
}

HRESULT OnnxruntimeEngine::RuntimeClassInitialize(OnnxruntimeEngineFactory* engine_factory,
                                                  UniqueOrtSession&& session,
                                                  IOrtSessionBuilder* session_builder) {
  engine_factory_ = engine_factory;
  session_ = std::move(session);
  session_builder_ = session_builder;
  return S_OK;
}

HRESULT OnnxruntimeEngine::LoadModel(_In_ IModel* model) {
  Microsoft::WRL::ComPtr<IOnnxruntimeModel> onnxruntime_model;
  RETURN_IF_FAILED(model->QueryInterface(IID_PPV_ARGS(&onnxruntime_model)));

  OrtModel* ort_model;
  RETURN_IF_FAILED(onnxruntime_model->DetachOrtModel(&ort_model));

  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();

  winml_adapter_api->SessionLoadAndPurloinModel(session_.get(), ort_model);

  return S_OK;
}

HRESULT OnnxruntimeEngine::Initialize() {
  RETURN_IF_FAILED(session_builder_->Initialize(session_.get()));
  return S_OK;
}

HRESULT OnnxruntimeEngine::RegisterGraphTransformers() {
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();
  winml_adapter_api->SessionRegisterGraphTransformers(session_.get());
  return S_OK;
}

HRESULT OnnxruntimeEngine::RegisterCustomRegistry(IMLOperatorRegistry* registry) {
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();
  winml_adapter_api->SessionRegisterCustomRegistry(session_.get(), registry);  
  return S_OK;
}

HRESULT OnnxruntimeEngine::EndProfiling() {
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();
  winml_adapter_api->SessionEndProfiling(session_.get());
  return S_OK;
}

HRESULT OnnxruntimeEngine::StartProfiling() {
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();

  OrtEnv* ort_env;
  engine_factory_->GetOrtEnvironment(&ort_env);

  winml_adapter_api->SessionStartProfiling(ort_env, session_.get());
  return S_OK;
}

HRESULT OnnxruntimeEngine::FlushContext() {
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();

  OrtExecutionProvider* ort_provider;
  winml_adapter_api->SessionGetExecutionProvider(session_.get(), 0, &ort_provider);

  winml_adapter_api->DmlExecutionProviderFlushContext(ort_provider);
  return S_OK;
}

HRESULT OnnxruntimeEngine::TrimUploadHeap() {
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();

  OrtExecutionProvider* ort_provider;
  winml_adapter_api->SessionGetExecutionProvider(session_.get(), 0, &ort_provider);

  winml_adapter_api->DmlExecutionProviderTrimUploadHeap(ort_provider);
  return S_OK;

}

HRESULT OnnxruntimeEngine::ReleaseCompletedReferences() {
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();

  OrtExecutionProvider* ort_provider;
  winml_adapter_api->SessionGetExecutionProvider(session_.get(), 0, &ort_provider);

  winml_adapter_api->DmlExecutionProviderReleaseCompletedReferences(ort_provider);
  return S_OK;
}

HRESULT OnnxruntimeEngine::CopyOneInputAcrossDevices(const char* input_name, const IValue* src, IValue** dest) {
  return E_NOTIMPL;
}

HRESULT OnnxruntimeEngine::Sync() {
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();

  OrtExecutionProvider* ort_provider;
  winml_adapter_api->SessionGetExecutionProvider(session_.get(), 0, &ort_provider);

  winml_adapter_api->ExecutionProviderSync(ort_provider);
  return S_OK;
}

OrtSession* OnnxruntimeEngine::UseOrtSession() {
  return session_.get();
}

HRESULT OnnxruntimeEngine::CreateTensorValue(int64_t* shape, size_t count, winml::TensorKind kind, _Out_ IValue** out) {
  auto ort_api = engine_factory_->UseOrtApi();
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();

  OrtExecutionProvider* ort_provider;
  winml_adapter_api->SessionGetExecutionProvider(session_.get(), 0, &ort_provider);

  OrtAllocator* ort_allocator;
  winml_adapter_api->GetProviderAllocator(ort_provider, &ort_allocator);
  auto unique_allocator = UniqueOrtAllocator(ort_allocator, winml_adapter_api->FreeProviderAllocator);  // the release here should probably not return anything

  OrtValue* ort_value;
  ort_api->CreateTensorAsOrtValue(unique_allocator.get(), shape, count, ONNXTensorElementDataTypeFromTensorKind(kind), &ort_value);
  auto unique_value = UniqueOrtValue(ort_value, ort_api->ReleaseValue);

  RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<OnnxruntimeValue>(out, engine_factory_.Get(), this, std::move(unique_value), std::move(unique_allocator)));
  return S_OK;
}

HRESULT OnnxruntimeEngine::CopyOneInputAcrossDevices(const char* name, IValue* src, IValue** out) {
  return E_NOTIMPL;
}

bool OnnxruntimeEngine::IsDmlSession() {
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();
  size_t num_providers;
  winml_adapter_api->SessionGetExecutionProvidersCount(session_.get(), &num_providers);
  return num_providers == 2; // There should be a better way to validate that the session is configured as to use dml
}

// TODO supposedly this doesnt work if it is not static
static std::shared_ptr<OnnxruntimeEnvironment> onnxruntime_environment_;

HRESULT OnnxruntimeEngineFactory::RuntimeClassInitialize() {
  const uint32_t ort_version = 1;
  const auto ort_api_base = OrtGetApiBase();
  ort_api_ = ort_api_base->GetApi(ort_version);
  winml_adapter_api_ = OrtGetWinMLAdapter(ort_api_);

  environment_ = onnxruntime_environment_ = PheonixSingleton<OnnxruntimeEnvironment>(ort_api_);
  return S_OK;
}

STDMETHODIMP OnnxruntimeEngineFactory::CreateModel(_In_ const char* model_path, _In_ size_t len, _Outptr_ IModel** out) {
  OrtModel* ort_model = nullptr;
  if (auto status = winml_adapter_api_->CreateModelFromPath(model_path, len, &ort_model)) {
    return E_FAIL;
  }

  auto model = UniqueOrtModel(ort_model, winml_adapter_api_->ReleaseModel);
  RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<OnnruntimeModel>(out, this, std::move(model)));
  return S_OK;
}

STDMETHODIMP OnnxruntimeEngineFactory::CreateModel(_In_ void* data, _In_ size_t size, _Outptr_ IModel** out) {
  OrtModel* ort_model = nullptr;
  if (auto status = winml_adapter_api_->CreateModelFromData(data, size, &ort_model)) {
    return E_FAIL;
  }

  auto model = UniqueOrtModel(ort_model, winml_adapter_api_->ReleaseModel);
  RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<OnnruntimeModel>(out, this, std::move(model)));
  return S_OK;
}

STDMETHODIMP OnnxruntimeEngineFactory::CreateEngineBuilder(_Outptr_ Windows::AI::MachineLearning::IEngineBuilder** out) {
  Microsoft::WRL::ComPtr<OnnxruntimeEngineBuilder> onnxruntime_engine_builder;
  RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<OnnxruntimeEngineBuilder>(&onnxruntime_engine_builder, this));
  RETURN_IF_FAILED(onnxruntime_engine_builder.CopyTo(out));
  return S_OK;
}

const OrtApi* OnnxruntimeEngineFactory::UseOrtApi() {
  return ort_api_;
}

const WinmlAdapterApi* OnnxruntimeEngineFactory::UseWinmlAdapterApi() {
  return winml_adapter_api_;
}

HRESULT OnnxruntimeEngineFactory::GetOrtEnvironment(OrtEnv** ort_env) {
  RETURN_IF_FAILED(environment_->GetOrtEnvironment(ort_env));
  return S_OK;
}

HRESULT OnnxruntimeEngineFactory::EnableDebugOutput(bool is_enabled) {
  RETURN_IF_FAILED(environment_->EnableDebugOutput(is_enabled));
  return S_OK;
}

HRESULT OnnxruntimeEngineFactory::CreateCustomRegistry(IMLOperatorRegistry** registry) {
  winml_adapter_api_->CreateCustomRegistry(registry);
  return S_OK;
}

STDAPI CreateOnnxruntimeEngineFactory(_Out_ Windows::AI::MachineLearning::IEngineFactory** engine_factory) {
  Microsoft::WRL::ComPtr<OnnxruntimeEngineFactory> onnxruntime_engine_factory;
  RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<OnnxruntimeEngineFactory>(&onnxruntime_engine_factory));
  RETURN_IF_FAILED(onnxruntime_engine_factory.CopyTo(engine_factory));
  return S_OK;
}

/* add these implementation pieces into the right places into the onnxruntime value/engine api calls
value->GetResource


value->IsCpu()
   
  bool LearningModelBinding::IsOfMapType(const Ort::Value& ort_value, TensorKind key_kind, TensorKind value_kind) {
    if (ort_value.GetTypeInfo().GetONNXType() != ONNX_TYPE_MAP)
      return false;

    ONNXTensorElementDataType onnx_key_type;
    ONNXTensorElementDataType onnx_value_type;

    WINML_THROW_IF_FAILED(adapter_->GetMapType(ort_value, &onnx_key_type, &onnx_value_type));

    if (onnx_key_type != GetONNXTensorElementDataType(key_kind))
      return false;

    if (onnx_value_type != GetONNXTensorElementDataType(value_kind))
      return false;

    return true;
  };

  bool LearningModelBinding::IsOfVectorMapType(const Ort::Value& ort_value, TensorKind key_kind, TensorKind value_kind) {
    if (ort_value.GetTypeInfo().GetONNXType() != ONNX_TYPE_SEQUENCE)
      return false;

    ONNXTensorElementDataType onnx_key_type;
    ONNXTensorElementDataType onnx_value_type;

    WINML_THROW_IF_FAILED(adapter_->GetVectorMapType(ort_value, &onnx_key_type, &onnx_value_type));

    if (onnx_key_type != GetONNXTensorElementDataType(key_kind))
      return false;

    if (onnx_value_type != GetONNXTensorElementDataType(value_kind))
      return false;

    return true;
  };

  bool LearningModelBinding::IsOfTensorType(const Ort::Value& ort_value, TensorKind kind) {
    return ort_value.GetTensorTypeAndShapeInfo().GetElementType() == GetONNXTensorElementDataType(kind);
  };

  gettensorshape
      uint32_t width = static_cast<uint32_t>(ort_value.GetTensorTypeAndShapeInfo().GetShape()[3]);
  uint32_t height = static_cast<uint32_t>(ort_value.GetTensorTypeAndShapeInfo().GetShape()[2]);
  uint32_t batchSize = static_cast<uint32_t>(ort_value.GetTensorTypeAndShapeInfo().GetShape()[0]);

  */