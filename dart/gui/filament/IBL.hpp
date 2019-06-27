/*
 * Copyright (c) 2011-2018, The DART development contributors
 * All rights reserved.
 *
 * The list of contributors can be found at:
 *   https://github.com/dartsim/dart/blob/master/LICENSE
 *
 * This file is provided under the following "BSD-style" License:
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 *   USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 *   AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *   POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef DART_GUI_FILAMENT_IBL_HPP_
#define DART_GUI_FILAMENT_IBL_HPP_

#include <filament/Texture.h>

#include <math/vec3.h>

#include <string>

#include "dart/gui/filament/Path.hpp"

namespace filament {
class Engine;
class IndexBuffer;
class IndirectLight;
class Material;
class MaterialInstance;
class Renderable;
class Texture;
class Skybox;
} // namespace filament

namespace dart {
namespace gui {
namespace flmt {

/// Implementation of Image Based Lighting (IBL)
class IBL
{
public:
  explicit IBL(filament::Engine& engine);
  ~IBL();

  bool loadFromDirectory(const utils::Path& path);
  bool loadFromKtx(const std::string& prefix);

  filament::IndirectLight* getIndirectLight() const noexcept
  {
    return mIndirectLight;
  }

  filament::Skybox* getSkybox() const noexcept
  {
    return mSkybox;
  }

private:
  bool loadCubemapLevel(
      filament::Texture** texture,
      const utils::Path& path,
      size_t level = 0,
      std::string const& levelPrefix = "") const;

  bool loadCubemapLevel(
      filament::Texture** texture,
      filament::Texture::PixelBufferDescriptor* outBuffer,
      filament::Texture::FaceOffsets* outOffsets,
      const utils::Path& path,
      size_t level = 0,
      std::string const& levelPrefix = "") const;

  filament::Engine& mEngine;

  filament::math::float3 mBands[9] = {};

  filament::Texture* mTexture = nullptr;
  filament::IndirectLight* mIndirectLight = nullptr;
  filament::Texture* mSkyboxTexture = nullptr;
  filament::Skybox* mSkybox = nullptr;
};

} // namespace flmt
} // namespace gui
} // namespace dart

#endif // DART_GUI_FILAMENT_IBL_HPP_
