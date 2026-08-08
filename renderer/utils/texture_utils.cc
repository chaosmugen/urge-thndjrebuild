// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "renderer/utils/texture_utils.h"

#include "Graphics/GraphicsAccessories/interface/GraphicsAccessories.hpp"

namespace renderer {

void CreateTexture2D(Diligent::IRenderDevice* device,
                     Diligent::ITexture** texture,
                     const std::string& name,
                     const base::Vec2i& size,
                     Diligent::USAGE usage,
                     Diligent::BIND_FLAGS bind_flags,
                     Diligent::CPU_ACCESS_FLAGS access,
                     Diligent::TEXTURE_FORMAT format) {
  Diligent::TextureDesc texture_desc;
  texture_desc.Name = name.c_str();
  texture_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
  texture_desc.Width = size.x;
  texture_desc.Height = size.y;
  texture_desc.Format = format;
  texture_desc.BindFlags = bind_flags;
  texture_desc.Usage = usage;
  texture_desc.CPUAccessFlags = access;
  texture_desc.ClearValue.Format = texture_desc.Format;
  std::memset(texture_desc.ClearValue.Color, 0,
              sizeof(texture_desc.ClearValue.Color));

  // Vulkan 不会自动清零新分配的显存（OpenGL 多数驱动会），未初始化的
  // SRV 纹理（如 tilemap atlas）会呈现随机内容导致花屏。这里显式传入
  // 全 0 初始数据，确保所有平台创建时显存都被初始化为透明。
  const uint32_t bytes_per_element =
      Diligent::GetTextureFormatAttribs(texture_desc.Format).GetElementSize();
  std::vector<uint8_t> zeros(static_cast<size_t>(size.x) * size.y *
                             bytes_per_element,
                             0);
  Diligent::TextureSubResData sub_res_data;
  sub_res_data.pData = zeros.data();
  sub_res_data.Stride = static_cast<uint32_t>(size.x) * bytes_per_element;
  Diligent::TextureData texture_data;
  texture_data.pSubResources = &sub_res_data;
  texture_data.NumSubresources = 1;

  device->CreateTexture(texture_desc, &texture_data, texture);
}

void CreateTexture2D(Diligent::IRenderDevice* device,
                     Diligent::ITexture** texture,
                     const std::string& name,
                     SDL_Surface* data,
                     Diligent::USAGE usage,
                     Diligent::BIND_FLAGS bind_flags,
                     Diligent::CPU_ACCESS_FLAGS access,
                     Diligent::TEXTURE_FORMAT format) {
  Diligent::TextureDesc texture_desc;
  texture_desc.Name = name.c_str();
  texture_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
  texture_desc.Width = data->w;
  texture_desc.Height = data->h;
  texture_desc.Format = format;
  texture_desc.BindFlags = bind_flags;
  texture_desc.Usage = usage;
  texture_desc.CPUAccessFlags = access;
  texture_desc.ClearValue.Format = texture_desc.Format;
  std::memset(texture_desc.ClearValue.Color, 0,
              sizeof(texture_desc.ClearValue.Color));

  Diligent::TextureSubResData texture_sub_res_data;
  texture_sub_res_data.pData = data->pixels;
  texture_sub_res_data.Stride = data->pitch;

  Diligent::TextureData texture_data;
  texture_data.pSubResources = &texture_sub_res_data;
  texture_data.NumSubresources = 1;

  device->CreateTexture(texture_desc, &texture_data, texture);
}

}  // namespace renderer