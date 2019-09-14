#pragma once
#include "gpu.h"
#include <sstream>
#include <string>
#include <vector>

class GPU_HW : public GPU
{
public:
  GPU_HW();
  virtual ~GPU_HW();

protected:
  struct HWVertex
  {
    s32 x;
    s32 y;
    u32 color;
    u16 texcoord;
    u16 padding;

    static std::tuple<u8, u8> DecodeTexcoord(u16 texcoord)
    {
      return std::make_tuple(static_cast<u8>(texcoord), static_cast<u8>(texcoord >> 8));
    }
    static u16 EncodeTexcoord(u8 x, u8 y) { return ZeroExtend16(x) | (ZeroExtend16(y) << 8); }
  };

  virtual void UpdateTexturePageTexture();

  bool IsFlushed() const { return !m_batch_vertices.empty(); }

  void DispatchRenderCommand(RenderCommand rc, u32 num_vertices) override;

  void CalcViewport(int* x, int* y, int* width, int* height);
  void CalcScissorRect(int* left, int* top, int* right, int* bottom);

  std::string GenerateVertexShader(bool textured);
  std::string GenerateFragmentShader(bool textured, bool blending);
  std::string GenerateScreenQuadVertexShader();
  std::string GenerateTexturePageProgram(TextureColorMode mode);

  std::vector<HWVertex> m_batch_vertices;
  RenderCommand m_batch_command = {};

private:
  void GenerateShaderHeader(std::stringstream& ss);

  void LoadVertices(RenderCommand rc, u32 num_vertices);
};
