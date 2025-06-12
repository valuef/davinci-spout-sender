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

using namespace OFX;

#define PARAM_SPOUT_SENDER_NAME "sender_name"
#define PLUGIN_NAME "Spout Sender"

class Image_Copier : public OFX::ImageProcessor
{
public:
  const OFX::Image* src_img;
  int pixel_stride;

  explicit Image_Copier(OFX::ImageEffect& p_Instance) : OFX::ImageProcessor(p_Instance) {
  }

  virtual void multiThreadProcessImages(OfxRectI wnd) {
    for (int y = wnd.y1; y < wnd.y2; ++y) {
      auto * dst_px = _dstImg->getPixelAddress(wnd.x1, y);
      auto * src_px = src_img->getPixelAddress(wnd.x1, y);

      auto width = wnd.x2 - wnd.x1;

      memcpy(dst_px, src_px, width * pixel_stride);
    }
  }
};

class Spout_Plugin : public ImageEffect {
public: 
  Clip *dst_clip;
  Clip *src_clip;

  StringParam *sender_name;

  // NOTE(valuef): We need to lazy initialize spoutDX otherwise the spout sender will not transmit any data when a project has been loaded unless we create a new one.
  // So we do this lazy loading with a unique ptr.
  // 2025-06-12
  std::unique_ptr<spoutDX> spout;
  Image_Copier copier;

  ~Spout_Plugin() {
    release_spout();
  }

  void release_spout() {
    if (spout) {
      spout->ReleaseSender();
      spout->CloseDirectX11();
      spout.reset();
      spout = 0;
    }
  }

  explicit Spout_Plugin(OfxImageEffectHandle handle) : ImageEffect(handle), copier(*this) {
    dst_clip = fetchClip(kOfxImageEffectOutputClipName);
    src_clip = fetchClip(kOfxImageEffectSimpleSourceClipName);
    sender_name = fetchStringParam("sender_name");
    sender_name->setEnabled(true);
  }

  void init_spout() {
    if (!spout) {
      spout = std::unique_ptr<spoutDX>(new spoutDX);

      std::string name;
      sender_name->getValue(name);
      spout->SetSenderName(name.c_str());
    }
  }

  virtual void render(const RenderArguments& args) {
    if (!src_clip || !dst_clip) {
      throwSuiteStatusException(kOfxStatErrBadHandle);
    }

    std::unique_ptr<const Image> src(src_clip->fetchImage(args.time));
    std::unique_ptr<Image> dst(dst_clip->fetchImage(args.time));

    auto depth = src->getPixelDepth();
    auto components = src->getPixelComponents();

    if (depth != dst->getPixelDepth() || components != dst->getPixelComponents()) {
      throwSuiteStatusException(kOfxStatErrImageFormat);
    }

    auto src_bounds = src->getBounds();
    auto dst_bounds = dst->getBounds();

    auto width = src_bounds.x2 - src_bounds.x1;
    auto height = src_bounds.y2 - src_bounds.y1;

    auto* src_px = src->getPixelData();
    auto* dst_px = dst->getPixelData();

    auto pixel_stride = 0;
    auto dx_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    auto dx_pitch = width * 4;
    if (depth == eBitDepthUByte) {
      if (components == ePixelComponentRGBA) {
        pixel_stride = 4;
        dx_pitch = width * pixel_stride;
        dx_format = DXGI_FORMAT_R8G8B8A8_UNORM;
      }
      else if (components == ePixelComponentRGB) {
        pixel_stride = 3;
        dx_pitch = width * 3;
        dx_format = DXGI_FORMAT_R8G8B8A8_UNORM; // TODO no RGB format
      }
      else if (components == ePixelComponentAlpha) {
        pixel_stride = 1;
        dx_pitch = width;
        dx_format = DXGI_FORMAT_R8_UNORM;
      }
    }
    else if (depth == eBitDepthUShort) {
      if (components == ePixelComponentRGBA) {
        pixel_stride = 4 * 2;
        dx_pitch = width * pixel_stride;
        dx_format = DXGI_FORMAT_R16G16B16A16_UNORM;
      }
      else if (components == ePixelComponentRGB) {
        pixel_stride = 3 * 2;
        dx_pitch = width * 3 * 2;
        dx_format = DXGI_FORMAT_R16G16B16A16_UNORM; // TODO no RGB format
      }
      else if (components == ePixelComponentAlpha) {
        pixel_stride = 2;
        dx_pitch = width * 2;
        dx_format = DXGI_FORMAT_R16_UNORM;
      }
    }
    else if (depth == eBitDepthHalf) {
      if (components == ePixelComponentRGBA) {
        pixel_stride = 4 * 2;
        dx_pitch = width * 4 * 2;
        dx_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      }
      else if (components == ePixelComponentRGB) {
        pixel_stride = 3 * 2;
        dx_pitch = width * 3 * 2;
        dx_format = DXGI_FORMAT_R16G16B16A16_FLOAT; // TODO no RGB format
      }
      else if (components == ePixelComponentAlpha) {
        pixel_stride = 2;
        dx_pitch = width * 2;
        dx_format = DXGI_FORMAT_R16_FLOAT;
      }
    }
    else if (depth == eBitDepthFloat) {
      if (components == ePixelComponentRGBA) {
        pixel_stride = 4 * 4;
        dx_pitch = width * 4 * 4;
        dx_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      }
      else if (components == ePixelComponentRGB) {
        pixel_stride = 3 * 4;
        dx_pitch = width * 3 * 4;
        dx_format = DXGI_FORMAT_R32G32B32A32_FLOAT; // TODO no RGB format
      }
      else if (components == ePixelComponentAlpha) {
        pixel_stride = 4;
        dx_pitch = width * 4;
        dx_format = DXGI_FORMAT_R32_FLOAT;
      }
    }

    init_spout();

    // NOTE(valuef): Modified spout.SendImage
    // 2025-06-12
    {
      spout->SetSenderFormat(dx_format);

      if (!spout->OpenDirectX11()) {
        spout->SpoutMessageBox("SpoutSender: Failed to open D3D11");
        return;
      }

      if(!spout->CheckSender(width, height, dx_format)) {
        spout->SpoutMessageBox("checksender failed");
        throwSuiteStatusException(kOfxStatErrImageFormat);
        return;
      }

      // Check the sender mutex for access the shared texture
      if (spout->frame.CheckTextureAccess(spout->m_pSharedTexture)) {
        // Update the shared texture resource with the pixel buffer
        spout->m_pImmediateContext->UpdateSubresource(spout->m_pSharedTexture, 0, NULL, src_px, dx_pitch, 0);
        // Flush the command queue because the shared texture has been updated on this device
        spout->m_pImmediateContext->Flush();
        // Signal a new frame while the mutex is locked
        spout->frame.SetNewFrame();
        // Allow access to the shared texture
        spout->frame.AllowTextureAccess(spout->m_pSharedTexture);
      }
    }

    copier.setDstImg(dst.get());
    copier.src_img = src.get();
    copier.pixel_stride = pixel_stride;
    copier.setRenderWindow(args.renderWindow);
    copier.process();
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
  SpoutPluginFactory() : PluginFactoryHelper("gay.value.SpoutSender", 1, 0) { }
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

    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(false);
    desc.setSupportsTiles(false);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setSupportsOpenCLRender(false);
    desc.setNoSpatialAwareness(true);
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
