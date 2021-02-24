#include "McRtcGui.h"

#include <Corrade/Utility/ConfigurationGroup.h>
#include <Magnum/MeshTools/Transform.h>
#include <Magnum/Primitives/Axis.h>
#include <Magnum/Primitives/Cone.h>
#include <Magnum/Primitives/Cube.h>
#include <Magnum/Primitives/Cylinder.h>
#include <Magnum/Primitives/Grid.h>
#include <Magnum/Primitives/Icosphere.h>
#include <Magnum/Primitives/Line.h>

#include "assets/Robot_Regular_ttf.h"

class ColoredDrawable : public SceneGraph::Drawable3D
{
public:
  explicit ColoredDrawable(Object3D & object,
                           Shaders::Phong & shader,
                           GL::Mesh & mesh,
                           const Color4 & color,
                           SceneGraph::DrawableGroup3D & group)
  : SceneGraph::Drawable3D{object, &group}, shader_(shader), mesh_(mesh), color_{color}
  {
  }

private:
  void draw(const Matrix4 & transformationMatrix, SceneGraph::Camera3D & camera) override
  {
    shader_.setDiffuseColor(color_)
        .setTransformationMatrix(transformationMatrix)
        .setNormalMatrix(transformationMatrix.normalMatrix())
        .setProjectionMatrix(camera.projectionMatrix())
        .draw(mesh_);
  }

  Shaders::Phong & shader_;
  GL::Mesh & mesh_;
  Color4 color_;
};

class TexturedDrawable : public SceneGraph::Drawable3D
{
public:
  explicit TexturedDrawable(Object3D & object,
                            Shaders::Phong & shader,
                            GL::Mesh & mesh,
                            GL::Texture2D & texture,
                            SceneGraph::DrawableGroup3D & group)
  : SceneGraph::Drawable3D{object, &group}, shader_(shader), mesh_(mesh), texture_(texture)
  {
  }

private:
  void draw(const Matrix4 & transformationMatrix, SceneGraph::Camera3D & camera) override
  {
    shader_.setTransformationMatrix(transformationMatrix)
        .setNormalMatrix(transformationMatrix.normalMatrix())
        .setProjectionMatrix(camera.projectionMatrix())
        .bindDiffuseTexture(texture_)
        .draw(mesh_);
  }

  Shaders::Phong & shader_;
  GL::Mesh & mesh_;
  GL::Texture2D & texture_;
};

struct Grid : public SceneGraph::Drawable3D
{
  Grid(Object3D & object, SceneGraph::DrawableGroup3D & drawables) : SceneGraph::Drawable3D(object, &drawables)
  {
    object.scale({5.0, 5.0, 5.0});
    shader_.setColor(0xffffff55_rgbaf);
    mesh_ = MeshTools::compile(Primitives::grid3DWireframe({9, 9}));
  }

  void draw(const Matrix4 & transformation, SceneGraph::Camera3D & camera) override
  {
    shader_.setTransformationProjectionMatrix(camera.projectionMatrix() * transformation).draw(mesh_);
  }

private:
  Shaders::Flat3D shader_;
  GL::Mesh mesh_;
};

McRtcGui::McRtcGui(const Arguments & arguments)
: Platform::Application{arguments, Configuration{}
                                       .setTitle("mc_rtc - Magnum based GUI")
                                       .setWindowFlags(Configuration::WindowFlag::Resizable
                                                       | Configuration::WindowFlag::Maximized)},
  client_(*this)
{
  {
    ImGui::CreateContext();

    ImGuiIO & io = ImGui::GetIO();
    ImFontConfig fontConfig;
    fontConfig.FontDataOwnedByAtlas = false;
    io.FontDefault = io.Fonts->AddFontFromMemoryTTF(Roboto_Regular_ttf, Roboto_Regular_ttf_len, 18.0f, &fontConfig);

    ImGui::StyleColorsLight();
    auto & style = ImGui::GetStyle();
    style.FrameRounding = 6.0f;
    auto & bgColor = style.Colors[ImGuiCol_WindowBg];
    bgColor.w = 0.5f;

    imgui_ = ImGuiIntegration::Context(*ImGui::GetCurrentContext(), Vector2{windowSize()} / dpiScaling(), windowSize(),
                                       framebufferSize());
  }

  /* Set up proper blending to be used by ImGui. There's a great chance
     you'll need this exact behavior for the rest of your scene. If not, set
     this only for the drawFrame() call. */
  GL::Renderer::setBlendEquation(GL::Renderer::BlendEquation::Add, GL::Renderer::BlendEquation::Add);
  GL::Renderer::setBlendFunction(GL::Renderer::BlendFunction::SourceAlpha,
                                 GL::Renderer::BlendFunction::OneMinusSourceAlpha);

  colorShader_.setAmbientColor(0x111111_rgbf).setSpecularColor(0xffffff_rgbf).setShininess(80.0f);
  textureShader_.setAmbientColor(0x111111_rgbf).setSpecularColor(0x111111_rgbf).setShininess(80.0f);

  /** Plugin */
  importer_ = manager_.loadAndInstantiate("AssimpImporter");
  importer_->configuration().setValue("ImportColladaIgnoreUpDirection", true);
  importer_->configuration().group("postprocess")->setValue("PreTransformVertices", true);

  /** Camera setup */
  {
    const Vector3 eye{2.5f, -1.5f, 1.5f};
    const Vector3 center{0.0f, 0.0f, 0.75f};
    const Vector3 up = Vector3::zAxis();
    camera_.emplace(scene_, eye, center, up, 60.0_degf, windowSize(), framebufferSize());
  }

  client_.connect("ipc:///tmp/mc_rtc_pub.ipc", "ipc:///tmp/mc_rtc_rep.ipc");
  client_.timeout(1.0);

  /** Grid */
  {
    new Grid{*new Object3D{&scene_}, drawables_};
  }

  axisMesh_ = MeshTools::compile(Primitives::axis3D());
  cubeMesh_ = MeshTools::compile(Primitives::cubeSolid());
  sphereMesh_ = MeshTools::compile(Primitives::icosphereSolid(2));
}

auto McRtcGui::importData(const std::string & path) -> ImportedMesh &
{
  auto it = importedData_.find(path);
  // FIXME Check the file hash to detect online changes
  if(it != importedData_.end())
  {
    return it->second;
  }
  auto & out = importedData_[path];
  if(!importer_->openFile(path))
  {
    return out;
  }
  out.textures_ = Containers::Array<Containers::Optional<GL::Texture2D>>{importer_->textureCount()};
  for(UnsignedInt i = 0; i < importer_->textureCount(); ++i)
  {
    Containers::Optional<Trade::TextureData> textureData = importer_->texture(i);
    if(!textureData || textureData->type() != Trade::TextureData::Type::Texture2D)
    {
      Warning{} << "Cannot load texture properties, skipping";
      continue;
    }

    Containers::Optional<Trade::ImageData2D> imageData = importer_->image2D(textureData->image());
    GL::TextureFormat format;
    if(imageData && imageData->format() == PixelFormat::RGB8Unorm)
      format = GL::TextureFormat::RGB8;
    else if(imageData && imageData->format() == PixelFormat::RGBA8Unorm)
      format = GL::TextureFormat::RGBA8;
    else
    {
      Warning{} << "Cannot load texture image, skipping";
      continue;
    }

    /* Configure the texture */
    GL::Texture2D texture;
    texture.setMagnificationFilter(textureData->magnificationFilter())
        .setMinificationFilter(textureData->minificationFilter(), textureData->mipmapFilter())
        .setWrapping(textureData->wrapping().xy())
        .setStorage(Math::log2(imageData->size().max()) + 1, format, imageData->size())
        .setSubImage(0, {}, *imageData)
        .generateMipmap();

    out.textures_[i] = std::move(texture);
  }
  /* Load all materials. Materials that fail to load will be NullOpt. The
   data will be stored directly in objects later, so save them only
   temporarily. */
  out.materials_ = Containers::Array<Containers::Optional<Trade::PhongMaterialData>>{importer_->materialCount()};
  for(UnsignedInt i = 0; i != importer_->materialCount(); ++i)
  {
    Containers::Optional<Trade::MaterialData> materialData = importer_->material(i);
    if(!materialData || !(materialData->types() & Trade::MaterialType::Phong))
    {
      Warning{} << "Cannot load material, skipping";
      continue;
    }

    out.materials_[i] = std::move(static_cast<Trade::PhongMaterialData &>(*materialData));
  }
  /* Load all meshes. Meshes that fail to load will be NullOpt. */
  out.meshes_ = Containers::Array<Containers::Optional<GL::Mesh>>{importer_->meshCount()};
  for(UnsignedInt i = 0; i != importer_->meshCount(); ++i)
  {
    Containers::Optional<Trade::MeshData> meshData = importer_->mesh(i);
    if(!meshData || !meshData->hasAttribute(Trade::MeshAttribute::Normal)
       || meshData->primitive() != MeshPrimitive::Triangles)
    {
      Warning{} << "Cannot load the mesh, skipping " << path.c_str();
      continue;
    }

    /* Compile the mesh */
    out.meshes_[i] = MeshTools::compile(*meshData);
  }
  if(importer_->defaultScene() != -1)
  {
    out.scene_ = importer_->scene(importer_->defaultScene());
    if(!out.scene_)
    {
      Error{} << "Cannot load scene from " << path.c_str();
    }
    else
    {
      out.objects_ = Containers::Array<Containers::Pointer<Trade::ObjectData3D>>{importer_->object3DCount()};
      for(UnsignedInt i = 0; i != importer_->object3DCount(); ++i)
      {
        out.objects_[i] = importer_->object3D(i);
      }
    }
  }
  return out;
}

void McRtcGui::loadMesh(const std::string & path, Object3D & parent, SceneGraph::DrawableGroup3D & group)
{
  auto & data = importData(path);
  if(data.scene_)
  {
    for(UnsignedInt objectId : data.scene_->children3D())
    {
      addObject(data, parent, group, objectId);
    }
  }
  else if(!data.meshes_.empty() && data.meshes_[0])
  {
    new ColoredDrawable{parent, colorShader_, *data.meshes_[0], 0xffffff_rgbf, drawables_};
  }
}

void McRtcGui::addObject(ImportedMesh & data, Object3D & parent, SceneGraph::DrawableGroup3D & group, UnsignedInt i)
{
  const Containers::Pointer<Trade::ObjectData3D> & objectData = data.objects_[i];
  if(!objectData)
  {
    Error{} << "Cannot import object, skipping";
    return;
  }

  /* Add the object to the scene and set its transformation */
  auto * object = new Object3D{&parent};
  object->setTransformation(objectData->transformation());

  /* Add a drawable if the object has a mesh and the mesh is loaded */
  if(objectData->instanceType() == Trade::ObjectInstanceType3D::Mesh && objectData->instance() != -1
     && data.meshes_[objectData->instance()])
  {
    const Int materialId = static_cast<const Trade::MeshObjectData3D *>(objectData.get())->material();

    /* Material not available / not loaded, use a default material */
    if(materialId == -1 || !data.materials_[materialId])
    {
      new ColoredDrawable{*object, colorShader_, *data.meshes_[objectData->instance()], 0xffffff_rgbf, group};
    }
    /* Textured material. If the texture failed to load, again just use a
       default colored material. */
    else if(data.materials_[materialId]->hasAttribute(Trade::MaterialAttribute::DiffuseTexture))
    {
      Containers::Optional<GL::Texture2D> & texture = data.textures_[data.materials_[materialId]->diffuseTexture()];
      if(texture)
      {
        new TexturedDrawable{*object, textureShader_, *data.meshes_[objectData->instance()], *texture, group};
      }
      else
      {
        new ColoredDrawable{*object, colorShader_, *data.meshes_[objectData->instance()], 0xffffff_rgbf, group};
      }
    }
    /* Color-only material */
    else
    {
      new ColoredDrawable{*object, colorShader_, *data.meshes_[objectData->instance()],
                          data.materials_[materialId]->diffuseColor(), group};
    }
  }

  /* Recursively add children */
  for(std::size_t id : objectData->children())
  {
    addObject(data, *object, group, id);
  }
}

void McRtcGui::drawEvent()
{
  GL::defaultFramebuffer.clear(GL::FramebufferClear::Color | GL::FramebufferClear::Depth);
  GL::Renderer::enable(GL::Renderer::Feature::Blending);

  client_.update();

  camera_->update();
  camera_->draw(drawables_);
  drawFrame({}, 0.1);

  imgui_.newFrame();
  ImGuizmo::BeginFrame();

  client_.draw3D();

  /* Enable text input, if needed */
  if(ImGui::GetIO().WantTextInput && !isTextInputActive())
  {
    startTextInput();
  }
  else if(!ImGui::GetIO().WantTextInput && isTextInputActive())
  {
    stopTextInput();
  }

  ImGuiIO & io = ImGui::GetIO();
  ImGuizmo::AllowAxisFlip(false);
  ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

  client_.draw2D();

  /* Update application cursor */
  imgui_.updateApplicationCursor(*this);

  /* Set appropriate states. If you only draw ImGui, it is sufficient to
     just enable blending and scissor test in the constructor. */
  GL::Renderer::enable(GL::Renderer::Feature::ScissorTest);
  GL::Renderer::disable(GL::Renderer::Feature::FaceCulling);
  GL::Renderer::disable(GL::Renderer::Feature::DepthTest);

  imgui_.drawFrame();

  /* Reset state. Only needed if you want to draw something else with
     different state after. */
  GL::Renderer::enable(GL::Renderer::Feature::DepthTest);
  GL::Renderer::enable(GL::Renderer::Feature::FaceCulling);
  GL::Renderer::disable(GL::Renderer::Feature::ScissorTest);

  swapBuffers();
  redraw();
}

void McRtcGui::viewportEvent(ViewportEvent & event)
{
  GL::defaultFramebuffer.setViewport({{}, event.framebufferSize()});

  imgui_.relayout(Vector2{event.windowSize()} / event.dpiScaling(), event.windowSize(), event.framebufferSize());

  camera_->reshape(event.windowSize(), event.framebufferSize());
}

void McRtcGui::keyPressEvent(KeyEvent & event)
{
  if(imgui_.handleKeyPressEvent(event))
  {
    return;
  }
}

void McRtcGui::keyReleaseEvent(KeyEvent & event)
{
  if(imgui_.handleKeyReleaseEvent(event)) return;
}

void McRtcGui::mousePressEvent(MouseEvent & event)
{
  if(imgui_.handleMousePressEvent(event))
  {
    return;
  }
  camera_->initTransformation(event.position());
  event.setAccepted();
  redraw();
}

void McRtcGui::mouseReleaseEvent(MouseEvent & event)
{
  if(imgui_.handleMouseReleaseEvent(event))
  {
    return;
  }
}

void McRtcGui::mouseMoveEvent(MouseMoveEvent & event)
{
  if(imgui_.handleMouseMoveEvent(event))
  {
    return;
  }
  if(!event.buttons())
  {
    return;
  }
  if(event.modifiers() & MouseMoveEvent::Modifier::Shift)
  {
    camera_->translate(event.position());
  }
  else
  {
    camera_->rotate(event.position());
  }
  event.setAccepted();
  redraw();
}

void McRtcGui::mouseScrollEvent(MouseScrollEvent & event)
{
  if(imgui_.handleMouseScrollEvent(event))
  {
    /* Prevent scrolling the page */
    event.setAccepted();
    return;
  }
  const auto delta = event.offset().y();
  if(Math::abs(delta) < 1.0e-2f)
  {
    return;
  }
  camera_->zoom(delta);
  event.setAccepted();
  redraw();
}

void McRtcGui::textInputEvent(TextInputEvent & event)
{
  if(imgui_.handleTextInputEvent(event)) return;
}

void McRtcGui::drawCube(Vector3 center, Matrix3 ori, Vector3 size, Color4 color)
{
  draw(cubeMesh_, color, Matrix4::from(ori * Matrix3::fromDiagonal(size / 2.0), center));
}

void McRtcGui::drawSphere(Vector3 center, float radius, Color4 color)
{
  draw(sphereMesh_, color, Matrix4::from(Matrix3{Math::IdentityInit, radius}, center));
}

void McRtcGui::drawLine(Vector3 start, Vector3 end, Color4 color, float /*thickness*/)
{
  // FIXME Write a shader to handle nice line drawing
  auto lineMesh = MeshTools::compile(Primitives::line3D(start, end));
  draw(lineMesh, color);
}

void McRtcGui::drawArrow(Vector3 start, Vector3 end, float shaft_diam, float head_diam, float head_len, Color4 color)
{
  Vector3 normal = end - start;
  float height = normal.length();
  if(height == 0.0f)
  {
    return;
  }
  normal /= height;
  if(head_len >= height)
  {
    head_len = height;
  }
  float shaft_len = height - head_len;
  auto theta = angle(normal, {0.0f, 1.0f, 0.0f});
  auto axis = cross(normal, {0.0f, 1.0f, 0.0f});
  if(axis.length() == 0.0f)
  {
    axis = {1, 0, 0};
  }
  axis = axis.normalized();
  if(shaft_len != 0 && shaft_diam != 0)
  {
    float r = shaft_diam / 2;
    auto shaftMesh =
        MeshTools::compile(Primitives::cylinderSolid(16, 32, 0.5f * shaft_len / r, Primitives::CylinderFlag::CapEnds));
    draw(shaftMesh, color,
         Matrix4::translation(start + 0.5f * shaft_len * normal) * Matrix4::rotation(-theta, axis)
             * Matrix4::scaling({r, r, r}));
  }
  if(head_len != 0 && head_diam != 0)
  {
    float r = head_diam / 2;
    auto headPrimitive = Primitives::coneSolid(64, 128, 0.5f * head_len / r, Primitives::ConeFlag::CapEnd);
    MeshTools::transformPointsInPlace(Matrix4::scaling({r, r, r}),
                                      headPrimitive.mutableAttribute<Vector3>(Trade::MeshAttribute::Position));
    auto headMesh = MeshTools::compile(headPrimitive);
    draw(headMesh, color,
         Matrix4::translation(start + (shaft_len + 0.5f * head_len) * normal) * Matrix4::rotation(-theta, axis));
  }
}

void McRtcGui::drawFrame(Matrix4 pos, float scale)
{
  auto & camera = camera_->camera();
  vertexShader_
      .setTransformationProjectionMatrix(camera.projectionMatrix() * camera.cameraMatrix() * pos
                                         * Matrix4::scaling(Vector3{scale}))
      .draw(axisMesh_);
}

void McRtcGui::draw(GL::Mesh & mesh, const Color4 & color, const Matrix4 & worldTransform)
{
  auto & camera = camera_->camera();
  Matrix4 transform = camera.cameraMatrix() * worldTransform;
  shader_.setDiffuseColor(color)
      .setAmbientColor(Color3::fromHsv({color.hue(), 1.0f, 0.3f}))
      .setTransformationMatrix(transform)
      .setNormalMatrix(transform.normalMatrix())
      .setProjectionMatrix(camera.projectionMatrix())
      .draw(mesh);
}

MAGNUM_APPLICATION_MAIN(McRtcGui)
