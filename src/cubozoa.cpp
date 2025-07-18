#include "cubozoa/cubozoa.h"

#include "cubozoa/net/cubozoa_net.h"
#include "renderer/cubozoa_irenderer_context.h"

#include <GLFW/glfw3.h>
#include <murmurhash/MurmurHash3.h>

namespace cbz {

// Application
static GLFWwindow *sWindow;
static std::shared_ptr<spdlog::logger> sLogger;

// Input
static std::array<bool, static_cast<uint32_t>(Key::eCount)> sKeyMap;
static void InputKeyCallback(GLFWwindow *, int key, int, int action, int) {
  if (key == GLFW_KEY_UNKNOWN)
    return;

  if (action == GLFW_PRESS) {
    sKeyMap[key] = true;
  } else if (action == GLFW_RELEASE) {
    sKeyMap[key] = false;
  }
}

void InputInit() { glfwSetKeyCallback(sWindow, InputKeyCallback); }

bool IsKeyDown(Key key) {
  if (static_cast<uint32_t>(key) >= static_cast<uint32_t>(Key::eCount)) {
    return false;
  }

  return sKeyMap[static_cast<uint32_t>(key)];
}

// Renderer
struct TransformData {
  float transform[16];
  float view[16];
  float proj[16];

  float inverseTransform[16];
  float inverseView[16];
};

static std::vector<ShaderProgramCommand> sShaderProgramCmds;
static uint32_t sNextShaderProgramCmdIdx;

static std::unique_ptr<cbz::IRendererContext> sRenderer;

static StructuredBufferHandle sTransformSBH;
static std::array<TransformData, MAX_COMMAND_SUBMISSIONS> sTransforms;

Result Init(InitDesc initDesc) {
  Result result = Result::eSuccess;

  sLogger = spdlog::stdout_color_mt("cbz");
  sLogger->set_level(spdlog::level::trace);
  sLogger->set_pattern("[%^%l%$][CBZ] %v");

  switch (initDesc.netStatus) {
  case NetworkStatus::eClient:
    result = net::initClient();
    break;
  case NetworkStatus::eHost:
    result = net::initServer();
    break;
  case NetworkStatus::eNone:
    result = Result::eNetworkFailure;
    break;
  }

  if (result != Result::eSuccess) {
    return result;
  };

  if (glfwInit() != GLFW_TRUE) {
    sLogger->critical("Failed to initialize glfw!");
    return Result::eGLFWError;
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
  sWindow = glfwCreateWindow(initDesc.width, initDesc.height, initDesc.name,
                             NULL, NULL);

  if (!sWindow) {
    sLogger->critical("Failed to create window!");
    glfwTerminate();
    return Result::eGLFWError;
  }

  InputInit();

  sRenderer = RendererContextCreate();
  if (sRenderer->init(initDesc.width, initDesc.height, sWindow) !=
      Result::eSuccess) {
    return Result::eFailure;
  }

  // Initialize transform array to identity
  TransformData data = {};
  data.transform[0] = 1;
  data.transform[5] = 1;
  data.transform[10] = 1;
  data.transform[15] = 1;
  sTransforms.fill(data);

  sTransformSBH = StructuredBufferCreate(
      UniformType::eMat4,
      MAX_COMMAND_SUBMISSIONS * (sizeof(TransformData) / (sizeof(float) * 16)),
      sTransforms.data());

  sShaderProgramCmds.resize(2);
  sNextShaderProgramCmdIdx = 0;

  return result;
}

VertexBufferHandle VertexBufferCreate(const VertexLayout &vertexLayout,
                                      uint32_t vertexCount, const void *data,
                                      const std::string &name) {
  VertexBufferHandle vbh = HandleProvider<VertexBufferHandle>::write(name);

  if (sRenderer->vertexBufferCreate(vbh, vertexLayout, vertexCount, data) !=
      Result::eSuccess) {
    HandleProvider<VertexBufferHandle>::free(vbh);
    return {CBZ_INVALID_HANDLE};
  }

  return vbh;
}

void VertexBufferSet(VertexBufferHandle vbh) {
  sShaderProgramCmds[sNextShaderProgramCmdIdx].program.graphics.vbh = vbh;
}

void VertexBufferDestroy(VertexBufferHandle vbh) {
  if (HandleProvider<VertexBufferHandle>::isValid(vbh)) {
    sRenderer->vertexBufferDestroy(vbh);
    HandleProvider<VertexBufferHandle>::free(vbh);
  }
}

IndexBufferHandle IndexBufferCreate(IndexFormat format, uint32_t count,
                                    const void *data, const std::string &name) {
  IndexBufferHandle ibh = HandleProvider<IndexBufferHandle>::write(name);

  if (sRenderer->indexBufferCreate(ibh, format, count, data) !=
      Result::eSuccess) {
    HandleProvider<IndexBufferHandle>::free(ibh);
    return {CBZ_INVALID_HANDLE};
  }

  return ibh;
}

void IndexBufferSet(IndexBufferHandle ibh) {
  sShaderProgramCmds[sNextShaderProgramCmdIdx].program.graphics.ibh = ibh;
}

void IndexBufferDestroy(IndexBufferHandle ibh) {
  if (HandleProvider<IndexBufferHandle>::isValid(ibh)) {
    sRenderer->indexBufferDestroy(ibh);
    HandleProvider<IndexBufferHandle>::free(ibh);
  }
}

StructuredBufferHandle StructuredBufferCreate(UniformType type,
                                              uint32_t elementCount,
                                              const void *data,
                                              const std::string &name) {
  StructuredBufferHandle sbh =
      HandleProvider<StructuredBufferHandle>::write(name);

  if (sRenderer->structuredBufferCreate(sbh, type, elementCount, data) !=
      Result::eSuccess) {

    HandleProvider<StructuredBufferHandle>::free(sbh);
    return {CBZ_INVALID_HANDLE};
  }

  return sbh;
}

void StructuredBufferUpdate(StructuredBufferHandle sbh, uint32_t elementCount,
                            const void *data, uint32_t offset) {
  sRenderer->structuredBufferUpdate(sbh, elementCount, data, offset);
}

void StructuredBufferSet(BufferSlot slot, StructuredBufferHandle sbh,
                         bool dynamic) {
  Binding binding = {};

  binding.type = BindingType::eStructuredBuffer;
  if (dynamic) {
    binding.type = BindingType::eRWStructuredBuffer;
  }

  binding.value.storageBuffer.slot = static_cast<uint8_t>(slot);
  binding.value.storageBuffer.handle = sbh;

  sShaderProgramCmds[sNextShaderProgramCmdIdx].bindings.push_back(binding);
}

void StructuredBufferDestroy(StructuredBufferHandle sbh) {
  if (HandleProvider<StructuredBufferHandle>::isValid(sbh)) {
    sRenderer->structuredBufferDestroy(sbh);
    HandleProvider<StructuredBufferHandle>::free(sbh);
  }
}

UniformHandle UniformCreate(const std::string &name, UniformType type,
                            uint16_t elementCount) {
  UniformHandle uh = HandleProvider<UniformHandle>::write(name);

  switch (type) {
  case UniformType::eVec4:
  case UniformType::eMat4: {
    if (sRenderer->uniformBufferCreate(uh, type, elementCount) !=
        Result::eSuccess) {
      HandleProvider<UniformHandle>::free(uh);
      return {CBZ_INVALID_HANDLE};
    }
  } break;
  default:
    break;
  }

  return uh;
}

void UniformSet(UniformHandle uh, void *data, uint16_t num) {
  if (!HandleProvider<UniformHandle>::isValid(uh)) {
    sLogger->warn("Attempting to set uniform with invalid handle!");
    return;
  }

  sRenderer->uniformBufferUpdate(uh, data, num);

  Binding binding = {};
  binding.type = BindingType::eUniformBuffer;
  binding.value.uniformBuffer.handle = uh;

  sShaderProgramCmds[sNextShaderProgramCmdIdx].bindings.push_back(binding);
}

void UniformDestroy(UniformHandle uh) {
  if (!HandleProvider<UniformHandle>::isValid(uh)) {
    sLogger->warn("Attempting to destroy uniform with invalid handle!");
    return;
  }

  sRenderer->uniformBufferDestroy(uh);
  HandleProvider<UniformHandle>::free(uh);
}

TextureHandle Texture2DCreate(TextureFormat format, uint32_t w, uint32_t h,
                              const std::string &name) {
  TextureHandle uh = HandleProvider<TextureHandle>::write(name);

  // Prefer uniform buffer
  if (sRenderer->textureCreate(uh, format, w, h, 1, TextureDimension::e2D) !=
      Result::eSuccess) {
    HandleProvider<TextureHandle>::free(uh);
    return {CBZ_INVALID_HANDLE};
  }

  return uh;
}

void Texture2DUpdate(TextureHandle th, void *data, uint32_t count) {
  sRenderer->textureUpdate(th, data, count);
}

static void SamplerBind(TextureSlot textureSlot, TextureBindingDesc desc) {
  Binding binding = {};
  binding.type = BindingType::eSampler;
  binding.value.sampler.slot = static_cast<uint8_t>(textureSlot) + 1;
  binding.value.sampler.handle = sRenderer->getSampler(desc);

  sShaderProgramCmds[sNextShaderProgramCmdIdx].bindings.push_back(binding);
}

void TextureSet(TextureSlot slot, TextureHandle th, TextureBindingDesc desc) {
  Binding binding = {};
  binding.type = BindingType::eTexture2D;
  binding.value.texture.slot = static_cast<uint8_t>(slot);
  binding.value.texture.handle = th;
  sShaderProgramCmds[sNextShaderProgramCmdIdx].bindings.push_back(binding);

  if (desc.addressMode != AddressMode::eCount) {
    SamplerBind(slot, desc);
  }
}

void TextureDestroy(TextureHandle th) {
  if (!HandleProvider<TextureHandle>::isValid(th)) {
    sLogger->warn("Attempting to destroy invalid 'TextureHandle'!");
    return;
  }

  sRenderer->textureDestroy(th);
  HandleProvider<TextureHandle>::free(th);
}

ShaderHandle ShaderCreate(const std::string &path, const std::string &name) {
  ShaderHandle sh = HandleProvider<ShaderHandle>::write(name);

  if (sRenderer->shaderCreate(sh, path) != Result::eSuccess) {
    sLogger->error("Failed to create shader module!");
    HandleProvider<ShaderHandle>::free(sh);
    return {CBZ_INVALID_HANDLE};
  }

  return sh;
}

void ShaderDestroy(ShaderHandle sh) {
  if (HandleProvider<ShaderHandle>::isValid(sh)) {
    sRenderer->shaderDestroy(sh);
    HandleProvider<ShaderHandle>::free(sh);
  }
}

GraphicsProgramHandle GraphicsProgramCreate(ShaderHandle sh,
                                            const std::string &name) {
  GraphicsProgramHandle gph =
      HandleProvider<GraphicsProgramHandle>::write(name);

  if (sRenderer->graphicsProgramCreate(gph, sh) != Result::eSuccess) {
    HandleProvider<GraphicsProgramHandle>::free(gph);
    return {CBZ_INVALID_HANDLE};
  }

  return gph;
}

void GraphicsProgramDestroy(GraphicsProgramHandle gph) {
  if (!HandleProvider<GraphicsProgramHandle>::isValid(gph)) {
    sLogger->warn("Attempting to destroy invalid 'GraphicsProgramHandle'!");
    return;
  }

  sRenderer->graphicsProgramDestroy(gph);
  HandleProvider<GraphicsProgramHandle>::free(gph);
}

ComputeProgramHandle ComputeProgramCreate(ShaderHandle sh,
                                          const std::string &name) {
  ComputeProgramHandle cph = HandleProvider<ComputeProgramHandle>::write(name);

  if (sRenderer->computeProgramCreate(cph, sh) != Result::eSuccess) {
    HandleProvider<ComputeProgramHandle>::free(cph);
    return {CBZ_INVALID_HANDLE};
  }

  return cph;
}

void ComputeProgramDestroy(ComputeProgramHandle cph) {
  if (HandleProvider<ComputeProgramHandle>::isValid(cph)) {
    sRenderer->computeProgramDestroy(cph);
    HandleProvider<ComputeProgramHandle>::free(cph);
  }
}

void TransformSet(float *transform) {
  memcpy(&sTransforms[sShaderProgramCmds.size()], transform,
         sizeof(float) * 16);
}

void Submit(uint8_t target, GraphicsProgramHandle gph) {
  // TODO : Target progrma compatiblity check.
  if (sShaderProgramCmds.size() > MAX_COMMAND_SUBMISSIONS) {
    sLogger->error("Application has exceeded maximum draw calls! Consider "
                   "batching or instancing.");
    return;
  }

  StructuredBufferSet(BufferSlot::e0, sTransformSBH);

  if (sNextShaderProgramCmdIdx >= MAX_COMMAND_BINDINGS) {
    sLogger->error("Draw called exceeding max uniform binds {}",
                   sNextShaderProgramCmdIdx);
    return;
  }

  ShaderProgramCommand *currentCommand =
      &sShaderProgramCmds[sNextShaderProgramCmdIdx];
  currentCommand->programType = TargetType::eGraphics;

  uint32_t uniformHash;
  MurmurHash3_x86_32(currentCommand->bindings.data(),
                     static_cast<uint32_t>(currentCommand->bindings.size()) *
                         sizeof(Binding),
                     0, &uniformHash);

  currentCommand->program.graphics.ph = gph;

  currentCommand->target = target;
  currentCommand->sortKey =
      (uint64_t)(gph.idx & 0xFFFF) << 48 |
      (uint64_t)(currentCommand->program.graphics.vbh.idx & 0xFFFF) << 32 |
      (uint64_t)(uniformHash & 0xFFFFFFFF);

  sNextShaderProgramCmdIdx++;
}

void Submit(uint8_t target, ComputeProgramHandle cph, uint32_t x, uint32_t y,
            uint32_t z) {
  // TODO : Target progrma compatiblity check.
  if (sShaderProgramCmds.size() > MAX_COMMAND_SUBMISSIONS) {
    sLogger->error("Application has exceeded maximum submits calls!");
    return;
  }

  if (sNextShaderProgramCmdIdx >= MAX_COMMAND_BINDINGS) {
    sLogger->error("Submit called exceeding max uniform binds {}",
                   sNextShaderProgramCmdIdx);
    return;
  }

  ShaderProgramCommand *currentCommand =
      &sShaderProgramCmds[sNextShaderProgramCmdIdx];
  currentCommand->programType = TargetType::eCompute;

  uint32_t uniformHash;
  MurmurHash3_x86_32(currentCommand->bindings.data(),
                     static_cast<uint32_t>(currentCommand->bindings.size()) *
                         sizeof(Binding),
                     0, &uniformHash);

  currentCommand->program.compute.x = x;
  currentCommand->program.compute.y = y;
  currentCommand->program.compute.z = z;
  currentCommand->program.compute.ph = cph;

  currentCommand->target = target;
  currentCommand->sortKey =
      (uint64_t)(cph.idx & 0xFFFF) << 48 |
      // (uint64_t)(currentCommand->program.graphics.vbh.idx & 0xFFFF) << 32 |
      (uint64_t)(uniformHash & 0xFFFFFFFF);

  sNextShaderProgramCmdIdx++;
}

bool Frame() {
  const uint32_t submissionCount = sNextShaderProgramCmdIdx;

  if (submissionCount > 0) {
    StructuredBufferUpdate(sTransformSBH, submissionCount, sTransforms.data());
  }

  std::sort(sShaderProgramCmds.begin(),
            sShaderProgramCmds.begin() + submissionCount,
            [](const ShaderProgramCommand &a, const ShaderProgramCommand &b) {
              if (a.target != b.target) {
                return a.target < b.target;
              }

              return a.sortKey < b.sortKey;
            });

  sRenderer->submitSorted(sShaderProgramCmds.data(), submissionCount);

  // Clear submissions
  for (uint32_t i = 0; i < sNextShaderProgramCmdIdx; i++) {
    sShaderProgramCmds[i].bindings.clear();
    sShaderProgramCmds[i].programType = TargetType::eNone;
    sShaderProgramCmds[i].sortKey = std::numeric_limits<uint64_t>::max();
  }
  sNextShaderProgramCmdIdx = 0;

  glfwPollEvents();

  return !glfwWindowShouldClose(sWindow);
}

void Shutdown() {
  StructuredBufferDestroy(sTransformSBH);

  sRenderer->shutdown();

  glfwDestroyWindow(sWindow);
  glfwTerminate();
}

void VertexLayout::begin(VertexStepMode mode) {
  if (attributes.size() > 0 || stride != 0) {
    spdlog::warn(
        "VertexLayout::begin() called with non empty attribute array!");
  }

  stepMode = mode;
  stride = 0;
}

void VertexLayout::push_attribute(VertexAttributeType, VertexFormat format) {
  attributes.push_back(
      {format, stride, static_cast<uint32_t>(attributes.size())});

  stride += VertexFormatGetSize(format);
}

void VertexLayout::end() {
  // for (VertexAttribute & attrib : attributes) { }
}

bool VertexLayout::operator==(const VertexLayout &other) const {
  if (attributes.size() != other.attributes.size()) {
    return false;
  }

  if (stride != other.stride) {
    return false;
  }

  if ((uint32_t)stepMode != (uint32_t)other.stepMode) {
    return false;
  }

  for (uint64_t i = 0; i < attributes.size(); i++) {
    if (attributes[i].shaderLocation != other.attributes[i].shaderLocation) {
      return false;
    };

    if (attributes[i].format != other.attributes[i].format) {
      return false;
    };
  }

  return true;
}

bool VertexLayout::operator!=(const VertexLayout &other) const {
  return !(*this == other);
}

} // namespace cbz
