/*
Davinci Resolve Spout Sender
Copyright (C) 2025 ValueFactory

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated 
documentation files (the “Software”), to deal in the Software without restriction, including without limitation 
the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, 
and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of 
the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED 
TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL 
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF 
CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
DEALINGS IN THE SOFTWARE.
*/

#include <stdio.h>

#include "ofxsImageEffect.h"
#include "ofxsInteract.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"
#include "ofxsLog.h"
#include "ofxDrawSuite.h"
#include "ofxsSupportPrivate.h"

#include <d3d11_1.h>
#include "SpoutDX.h"

#include <wrl.h>
using namespace Microsoft::WRL;

#pragma comment(lib, "cudart.lib")

#if defined(_DEBUG)
  #define DEBUG_BREAK __debugbreak()
#else
  #define DEBUG_BREAK
#endif


#include <cuda_d3d11_interop.h>
#include <cuda_runtime.h>

using namespace OFX;

#define PARAM_SPOUT_SENDER_NAME "sender_name"

#if defined(_DEBUG)
  #define PLUGIN_NAME "SpoutSender_dev"
  #define PLUGIN_ID "gay.value.SpoutSender_dev"
  #define PLUGIN_MAJOR 1
  #define PLUGIN_MINOR 0
#else
  #define PLUGIN_NAME "SpoutSender"
  #define PLUGIN_ID "gay.value.SpoutSender"
  #define PLUGIN_MAJOR 1
  #define PLUGIN_MINOR 1
#endif

class Image_Copier : public OFX::ImageProcessor
{
public:
  const OFX::Image* src_img;
  int pixel_stride;

  explicit Image_Copier(OFX::ImageEffect& p_Instance) : OFX::ImageProcessor(p_Instance) {
  }

  virtual void multiThreadProcessImages(OfxRectI wnd) {
    for (int y = wnd.y1; y < wnd.y2; ++y) {
      auto* dst_px = _dstImg->getPixelAddress(wnd.x1, y);
      auto* src_px = src_img->getPixelAddress(wnd.x1, y);

      auto width = wnd.x2 - wnd.x1;

      memcpy(dst_px, src_px, width * pixel_stride);
    }
  }
};

void check_d3d11_error(HRESULT hr) {
  if (FAILED(hr)) {
    DEBUG_BREAK;
    throwSuiteStatusException(kOfxStatErrImageFormat);
  }
}

void check_cuda_error(cudaError_t result) {
  if (result != cudaSuccess) {
    auto err = cudaGetErrorString(result);
    DEBUG_BREAK;
    throwSuiteStatusException(kOfxStatErrImageFormat);
  }
}

void invalid_format() {
  DEBUG_BREAK;
  throwSuiteStatusException(kOfxStatErrImageFormat);
}


class Spout_Plugin : public ImageEffect {
public: 


  // COMMON
  
  // NOTE(valuef): We need to lazy initialize spoutDX otherwise the spout sender will not transmit any data when a project has been loaded unless we create a new one.
  // So we do this lazy loading with a unique ptr.
  // 2025-06-12
  std::unique_ptr<spoutDX> spout;

  Clip* dst_clip;
  Clip* src_clip;

  StringParam* sender_name;

  // CPU
  std::unique_ptr<Image_Copier> copier;

  // CUDA
  cudaGraphicsResource* cuda_in_tex;
  cudaGraphicsResource* cuda_out_tex;

  D3D11_TEXTURE2D_DESC in_tex_desc = {};
  ComPtr<ID3D11Texture2D> in_tex;
  ComPtr<ID3D11ShaderResourceView> in_srv;

  bool was_using_cuda;

  ~Spout_Plugin() {
    release_spout();
    cleanup_cuda();
  }

  explicit Spout_Plugin(OfxImageEffectHandle handle) : ImageEffect(handle) {
    dst_clip = fetchClip(kOfxImageEffectOutputClipName);
    src_clip = fetchClip(kOfxImageEffectSimpleSourceClipName);
    sender_name = fetchStringParam("sender_name");
    sender_name->setEnabled(true);
  }

  void release_spout() {
    if (spout) {
      spout->ReleaseSender();
      spout->CloseDirectX11();
      spout.reset();
      spout = 0;
    }
  }

  void init_spout() {
    if (!spout) {
      spout = std::unique_ptr<spoutDX>(new spoutDX);

      std::string name;
      sender_name->getValue(name);
      spout->SetSenderName(name.c_str());
    }
  }

  void cleanup_cuda() {
    if (cuda_in_tex) {
      cudaGraphicsUnregisterResource(cuda_in_tex);
      cuda_in_tex = 0;
    }
    if (cuda_out_tex) {
      cudaGraphicsUnregisterResource(cuda_out_tex);
      cuda_out_tex = 0;
    }
  }

  virtual void render(const RenderArguments& args) {
    if (!src_clip || !dst_clip) {
      DEBUG_BREAK;
      throwSuiteStatusException(kOfxStatErrBadHandle);
    }

    std::unique_ptr<Image> src(src_clip->fetchImage(args.time));
    std::unique_ptr<Image> dst(dst_clip->fetchImage(args.time));

    auto depth = src->getPixelDepth();
    auto components = src->getPixelComponents();

    if (depth != dst->getPixelDepth() || components != dst->getPixelComponents()) {
      invalid_format();
    }

    auto src_bounds = src->getBounds();
    auto dst_bounds = dst->getBounds();

    auto src_width = src_bounds.x2 - src_bounds.x1;
    auto src_height = src_bounds.y2 - src_bounds.y1;

    auto dst_width = dst_bounds.x2 - dst_bounds.x1;
    auto dst_height = dst_bounds.y2 - dst_bounds.y1;

    if (src_width != dst_width || src_height != dst_height) {
      invalid_format();
    }

    auto* src_px = src->getPixelData();
    auto* dst_px = dst->getPixelData();

    auto pixel_size_bytes = 0;
    auto dx_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (depth == eBitDepthUByte) {
      if (components == ePixelComponentRGBA) {
        pixel_size_bytes = 4;
        dx_format = DXGI_FORMAT_R8G8B8A8_UNORM;
      }
      else if (components == ePixelComponentRGB) {
        invalid_format();
      }
      else if (components == ePixelComponentAlpha) {
        pixel_size_bytes = 1;
        dx_format = DXGI_FORMAT_R8_UNORM;
      }
      else {
        invalid_format();
      }
    }
    else if (depth == eBitDepthUShort) {
      if (components == ePixelComponentRGBA) {
        pixel_size_bytes = 4 * 2;
        dx_format = DXGI_FORMAT_R16G16B16A16_UNORM;
      }
      else if (components == ePixelComponentRGB) {
        invalid_format();
      }
      else if (components == ePixelComponentAlpha) {
        pixel_size_bytes = 2;
        dx_format = DXGI_FORMAT_R16_UNORM;
      }
    }
    else if (depth == eBitDepthHalf) {
      if (components == ePixelComponentRGBA) {
        pixel_size_bytes = 4 * 2;
        dx_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      }
      else if (components == ePixelComponentRGB) {
        invalid_format();
      }
      else if (components == ePixelComponentAlpha) {
        pixel_size_bytes = 2;
        dx_format = DXGI_FORMAT_R16_FLOAT;
      }
      else {
        invalid_format();
      }
    }
    else if (depth == eBitDepthFloat) {
      if (components == ePixelComponentRGBA) {
        pixel_size_bytes = 4 * 4;
        dx_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      }
      else if (components == ePixelComponentRGB) {
        invalid_format();
      }
      else if (components == ePixelComponentAlpha) {
        pixel_size_bytes = 4;
        dx_format = DXGI_FORMAT_R32_FLOAT;
      }
      else {
        invalid_format();
      }
    }
    else {
      invalid_format();
    }

    auto stream = (cudaStream_t)args.pCudaStream;
    auto use_cuda = stream != nullptr;
    auto started_using_cuda = use_cuda && !was_using_cuda;

    init_spout();

    if (!use_cuda && !copier) {
      copier = std::unique_ptr<Image_Copier>(new Image_Copier(*this));
    }

    if (!spout->OpenDirectX11()) {
      spout->SpoutMessageBox("Failed to open D3D11.");
      throwSuiteStatusException(kOfxStatErrImageFormat);
      return;
    }
    
    if(started_using_cuda || in_tex_desc.Format != dx_format || in_tex_desc.Width != src_width || in_tex_desc.Height != src_height) {

      HRESULT hr = S_OK;
      {
        in_tex_desc.Width = src_width;
        in_tex_desc.Height = src_height;
        in_tex_desc.MipLevels = 1;
        in_tex_desc.ArraySize = 1;
        in_tex_desc.Format = dx_format;
        in_tex_desc.SampleDesc.Count = 1;
        in_tex_desc.Usage = D3D11_USAGE_DEFAULT;
        in_tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (!use_cuda) {
          in_tex_desc.Usage = D3D11_USAGE_DYNAMIC;
          in_tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        }

        hr = spout->m_pd3dDevice->CreateTexture2D(&in_tex_desc, 0, in_tex.ReleaseAndGetAddressOf());
        check_d3d11_error(hr);

        hr = spout->m_pd3dDevice->CreateShaderResourceView(in_tex.Get(), 0, in_srv.ReleaseAndGetAddressOf());
        check_d3d11_error(hr);

        if (use_cuda) {
          auto result = cudaGraphicsD3D11RegisterResource(&cuda_in_tex, in_tex.Get(), cudaGraphicsRegisterFlagsNone);
          check_cuda_error(result);
        }
      }
    }


    // NOTE(valuef): Modified spout.SendImage
    // 2025-06-12
    {
      spout->SetSenderFormat(dx_format);

      if(!spout->CheckSender(src_width, src_height, dx_format)) {
        spout->SpoutMessageBox("checksender failed");
        throwSuiteStatusException(kOfxStatErrImageFormat);
        return;
      }

      // Check the sender mutex for access the shared texture
      if (spout->frame.CheckTextureAccess(spout->m_pSharedTexture)) {

        auto pitch = src_width * pixel_size_bytes;

        if (use_cuda) {
          auto result = cudaGraphicsMapResources(1, &cuda_in_tex, stream);
          check_cuda_error(result);

          cudaArray* cuda_array;
          result = cudaGraphicsSubResourceGetMappedArray(&cuda_array, cuda_in_tex, 0, 0);
          check_cuda_error(result);

          result = cudaMemcpy2DToArrayAsync(cuda_array, 0, 0, src_px, pitch, pitch, src_height, cudaMemcpyDeviceToDevice, stream);
          check_cuda_error(result);

          result = cudaGraphicsUnmapResources(1, &cuda_in_tex, stream);
          check_cuda_error(result);

          spout->m_pImmediateContext->CopySubresourceRegion(spout->m_pSharedTexture, 0, 0, 0, 0, in_tex.Get(), 0, 0);
        }
        else {
          // TODO : crashes when going from GPU -> CPU mode
          // 
          // Update the shared texture resource with the pixel buffer
          spout->m_pImmediateContext->UpdateSubresource(spout->m_pSharedTexture, 0, NULL, src_px, pitch, 0);
        }

        // Flush the command queue because the shared texture has been updated on this device
        spout->m_pImmediateContext->Flush();
        // Signal a new frame while the mutex is locked
        spout->frame.SetNewFrame();
        // Allow access to the shared texture
        spout->frame.AllowTextureAccess(spout->m_pSharedTexture);
      }
    }

    if (use_cuda) {
      auto pitch = dst_width * pixel_size_bytes;
      auto result = cudaMemcpy2DAsync(dst_px, pitch, src_px, pitch, pitch, dst_height, cudaMemcpyDeviceToDevice, stream);
      check_cuda_error(result);
    }
    else {
      copier->setDstImg(dst.get());
      copier->src_img = src.get();
      copier->pixel_stride = pixel_size_bytes;
      copier->setRenderWindow(args.renderWindow);
      copier->process();
    }

    if (use_cuda) {
      was_using_cuda = true;
    }
  }


  virtual bool isIdentity(IsIdentityArguments& args, Clip*& clip, double& time) {
    return false;
  }

  virtual void getClipPreferences(ClipPreferencesSetter& pref) override {
    pref.setClipComponents(*src_clip, ePixelComponentRGBA);
    pref.setClipComponents(*dst_clip, ePixelComponentRGBA);
    pref.setClipBitDepth(*src_clip, eBitDepthUByte);
    pref.setClipBitDepth(*dst_clip, eBitDepthUByte);
  }

  virtual void changedParam(const InstanceChangedArgs& args, const std::string& param_name) override {
    if (param_name == PARAM_SPOUT_SENDER_NAME) {
      // NOTE(valuef): SetSenderName does not update the name of the sender. It's set and then it's constant.
      // So we need to re-create the spout sender to update the name.
      // 2025-06-12
      release_spout();
      init_spout();
    }
  }
};

class SpoutPluginFactory : public PluginFactoryHelper<SpoutPluginFactory> {
public:
  SpoutPluginFactory() : PluginFactoryHelper(PLUGIN_ID, PLUGIN_MAJOR, PLUGIN_MINOR) { }
  virtual void load() {
  }
  virtual void unload() {
  }

  virtual void describe(ImageEffectDescriptor& desc) {
    desc.setLabels(PLUGIN_NAME, PLUGIN_NAME, PLUGIN_NAME);
    desc.setPluginGrouping("Filter");
    desc.setPluginDescription("Sends the current clip over to Spout");

    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);

    desc.addSupportedBitDepth(eBitDepthFloat);
    // TODO seems like DepthHalf crashes at subresource copy on cpu. Does spout not support it?
    //desc.addSupportedBitDepth(eBitDepthHalf); // TODO test
    //desc.addSupportedBitDepth(eBitDepthUByte); // TODO test
    //desc.addSupportedBitDepth(eBitDepthUShort);// TODO test

    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(false);
    desc.setSupportsTiles(false);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setNoSpatialAwareness(true);

    desc.setSupportsCudaRender(true);
    desc.setSupportsCudaStream(true);
  }

  virtual void describeInContext(ImageEffectDescriptor& desc, ContextEnum context) {
    auto* src_clip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    src_clip->addSupportedComponent(ePixelComponentRGBA);
    src_clip->setTemporalClipAccess(false);
    src_clip->setSupportsTiles(false);
    src_clip->setIsMask(false);

    auto* dst_clip = desc.defineClip(kOfxImageEffectOutputClipName);
    dst_clip->addSupportedComponent(ePixelComponentRGBA);
    dst_clip->addSupportedComponent(ePixelComponentAlpha);
    dst_clip->setSupportsTiles(false);

    {
      auto* param = desc.defineStringParam(PARAM_SPOUT_SENDER_NAME);
      param->setLabels("Sender Name", "Sender Name", "Sender Name");
      param->setDefault("Davinci Spout");
      param->setAnimates(false);
    }
  }

  virtual ImageEffect* createInstance(OfxImageEffectHandle handle, ContextEnum context) {
    return new Spout_Plugin(handle);
  }
};

void OFX::Plugin::getPluginIDs(PluginFactoryArray& factory_array) {
  static SpoutPluginFactory factory;
  factory_array.push_back(&factory);
}
