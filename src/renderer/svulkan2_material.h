#pragma once
#include "renderer/render_interface.h"
#include <svulkan2/core/context.h>
#include <svulkan2/renderer/renderer.h>
#include <svulkan2/scene/scene.h>

namespace sapien {
namespace Renderer {

class SVulkan2Texture : public IPxrTexture {
  std::shared_ptr<svulkan2::resource::SVTexture> mTexture;

public:
  explicit SVulkan2Texture(std::shared_ptr<svulkan2::resource::SVTexture> texture);
  [[nodiscard]] virtual int getMipmapLevels() const override;
  [[nodiscard]] virtual int getWidth() const override;
  [[nodiscard]] virtual int getHeight() const override;
  [[nodiscard]] virtual int getChannels() const override;
  [[nodiscard]] virtual Type::Enum getType() const override;
  [[nodiscard]] virtual AddressMode::Enum getAddressMode() const override;
  [[nodiscard]] virtual FilterMode::Enum getFilterMode() const override;
  [[nodiscard]] virtual std::string getFilename() const override;

  [[nodiscard]] inline std::shared_ptr<svulkan2::resource::SVTexture> getTexture() const {
    return mTexture;
  }
};

class SVulkan2Material : public IPxrMaterial {
  std::shared_ptr<svulkan2::resource::SVMetallicMaterial> mMaterial;

public:
  explicit SVulkan2Material(std::shared_ptr<svulkan2::resource::SVMetallicMaterial> material);
  void setBaseColor(std::array<float, 4> color) override;
  [[nodiscard]] std::array<float, 4> getBaseColor() const override;
  void setRoughness(float roughness) override;
  [[nodiscard]] float getRoughness() const override;
  void setSpecular(float specular) override;
  [[nodiscard]] float getSpecular() const override;
  void setMetallic(float metallic) override;
  [[nodiscard]] float getMetallic() const override;

  void setDiffuseTexture(std::shared_ptr<IPxrTexture> texture) override;
  [[nodiscard]] std::shared_ptr<IPxrTexture> getDiffuseTexture() const override;
  void setRoughnessTexture(std::shared_ptr<IPxrTexture> texture) override;
  [[nodiscard]] std::shared_ptr<IPxrTexture> getRoughnessTexture() const override;
  void setMetallicTexture(std::shared_ptr<IPxrTexture> texture) override;
  [[nodiscard]] std::shared_ptr<IPxrTexture> getMetallicTexture() const override;
  void setNormalTexture(std::shared_ptr<IPxrTexture> texture) override;
  [[nodiscard]] std::shared_ptr<IPxrTexture> getNormalTexture() const override;

  [[nodiscard]] std::shared_ptr<svulkan2::resource::SVMetallicMaterial> getMaterial() const {
    return mMaterial;
  }
};

} // namespace Renderer
} // namespace sapien