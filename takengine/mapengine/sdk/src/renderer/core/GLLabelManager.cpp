#include "renderer/core/GLLabelManager.h"

#include "core/Ellipsoid.h"
#include "feature/LegacyAdapters.h"
#include "math/Vector4.h"
#include "renderer/GL.h"
#include "renderer/GLES20FixedPipeline.h"
#include "renderer/GLText2.h"
#include "renderer/core/GLLabel.h"
#include "renderer/core/GLMapRenderGlobals.h"
#include "renderer/core/GLMapView2.h"
#include "thread/Mutex.h"
#include "util/ConfigOptions.h"

using namespace TAK::Engine::Renderer::Core;

using namespace TAK::Engine::Core;
using namespace TAK::Engine::Math;
using namespace TAK::Engine::Renderer;
using namespace TAK::Engine::Thread;
using namespace TAK::Engine::Util;

using namespace atakmap::renderer;

namespace {
GLText2* defaultText(nullptr);
}  // namespace

float GLLabelManager::defaultFontSize = 0;

GLLabelManager::GLLabelManager()
    : labelRotation(0.0),
      absoluteLabelRotation(false),
      labelFadeTimer(-1LL),
      map_idx_(0),
      always_render_idx_(NO_ID),
      draw_version_(-1),
      replace_labels_(true),
      visible_(true) {
    label_priorities_.insert(std::make_pair(Priority::TEP_High, std::set<uint32_t>()));
    label_priorities_.insert(std::make_pair(Priority::TEP_Standard, std::set<uint32_t>()));
    label_priorities_.insert(std::make_pair(Priority::TEP_Low, std::set<uint32_t>()));
}

GLLabelManager::~GLLabelManager() { this->stop(); }

void GLLabelManager::resetFont() NOTHROWS {
    Lock lock(mutex_);

    draw_version_ = -1;
    defaultFontSize = 0;
    defaultText = nullptr;
}

uint32_t GLLabelManager::addLabel(GLLabel& label) NOTHROWS {
    Lock lock(mutex_);

    draw_version_ = -1;

    replace_labels_ = true;
    label_priorities_[label.priority_].insert(map_idx_);
    labels_[map_idx_] = std::move(label);
    return map_idx_++;
}

void GLLabelManager::removeLabel(const uint32_t id) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    replace_labels_ = true;
    label_priorities_[labels_[id].priority_].erase(id);
    labels_.erase(id);

    if (always_render_idx_ == id) always_render_idx_ = NO_ID;
}

void GLLabelManager::setGeometry(const uint32_t id, const TAK::Engine::Feature::Geometry2& geometry) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    replace_labels_ = true;
    labels_[id].setGeometry(geometry);
}

void GLLabelManager::setAltitudeMode(const uint32_t id, const TAK::Engine::Feature::AltitudeMode altitude_mode) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    replace_labels_ = true;
    labels_[id].setAltitudeMode(altitude_mode);
}

void GLLabelManager::setText(const uint32_t id, TAK::Engine::Port::String text) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    replace_labels_ = true;
    labels_[id].setText(text);
}

void GLLabelManager::setTextFormat(const uint32_t id, const TextFormatParams* fmt) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    replace_labels_ = true;

    // RWI - If it's the default font just reset to null
    if (fmt == nullptr ||
        (fmt->size == defaultFontSize && fmt->fontName == nullptr && !fmt->bold && !fmt->italic && !fmt->underline && !fmt->strikethrough))
        labels_[id].setTextFormat(nullptr);
    else
        labels_[id].setTextFormat(fmt);
}

void GLLabelManager::setVisible(const uint32_t id, bool visible) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    replace_labels_ = true;
    labels_[id].setVisible(visible);
}

void GLLabelManager::setAlwaysRender(const uint32_t id, bool always_render) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    replace_labels_ = true;
    labels_[id].setAlwaysRender(always_render);
    if (always_render)
        always_render_idx_ = id;
    else if (always_render_idx_ == id)
        always_render_idx_ = NO_ID;
}

void GLLabelManager::setMaxDrawResolution(const uint32_t id, double max_draw_resolution) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    replace_labels_ = true;
    labels_[id].setMaxDrawResolution(max_draw_resolution);
}

void GLLabelManager::setAlignment(const uint32_t id, TextAlignment alignment) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    labels_[id].setAlignment(alignment);
}

void GLLabelManager::setVerticalAlignment(const uint32_t id, VerticalAlignment vertical_alignment) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    labels_[id].setVerticalAlignment(vertical_alignment);
}

void GLLabelManager::setDesiredOffset(const uint32_t id, const TAK::Engine::Math::Point2<double>& desired_offset) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    labels_[id].setDesiredOffset(desired_offset);
}

void GLLabelManager::setColor(const uint32_t id, int color) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    labels_[id].setColor(color);
}

void GLLabelManager::setBackColor(const uint32_t id, int color) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    labels_[id].setBackColor(color);
}

void GLLabelManager::setFill(const uint32_t id, bool fill) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    labels_[id].setFill(fill);
}

void GLLabelManager::setRotation(const uint32_t id, const float rotation, const bool absolute) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    draw_version_ = -1;

    labels_[id].setRotation(rotation, absolute);
}

void GLLabelManager::getSize(const uint32_t id, atakmap::math::Rectangle<double>& size_rect) NOTHROWS {
    Lock lock(mutex_);

    if (map_idx_ <= id) return;

    size_rect = labels_[id].labelRect;

    if (size_rect.width == 0 && size_rect.height == 0) {
        GLText2* gltext = labels_[id].gltext_;
        if (!gltext)
            gltext = getDefaultText();
        if (gltext) {
            size_rect.width = gltext->getTextFormat().getStringWidth(labels_[id].text_.c_str());
            size_rect.height = gltext->getTextFormat().getStringHeight(labels_[id].text_.c_str());
        }
    }
}

void GLLabelManager::setPriority(const uint32_t id, const Priority priority) NOTHROWS {
    Lock lock(mutex_);

    for (auto it = label_priorities_.begin(); it != label_priorities_.end(); it++) {
        if (it->first == priority) continue;

        if (it->second.find(id) != it->second.end()) {
            it->second.erase(id);
        }
    }
    label_priorities_[priority].insert(id);

    labels_[id].setPriority(priority);
}

void GLLabelManager::setVisible(bool visible) NOTHROWS {
    Lock lock(mutex_);

    visible_ = visible;
}

void GLLabelManager::draw(const GLGlobeBase& view, const int render_pass) NOTHROWS {
    Lock lock(mutex_);

    if (labels_.empty() || !visible_) return;

    defaultText = getDefaultText();

    if (this->batch_.get() == nullptr) {
        this->batch_.reset(new GLRenderBatch2(0xFFFF));
    }

    try {
        GLES20FixedPipeline::getInstance()->glMatrixMode(GLES20FixedPipeline::MatrixMode::MM_GL_MODELVIEW);
        GLES20FixedPipeline::getInstance()->glPushMatrix();

        GLES20FixedPipeline::getInstance()->glMatrixMode(GLES20FixedPipeline::MatrixMode::MM_GL_PROJECTION);
        GLES20FixedPipeline::getInstance()->glPushMatrix();
        GLES20FixedPipeline::getInstance()->glOrthof((float)view.renderPass->left, (float)view.renderPass->right, (float)view.renderPass->bottom, (float)view.renderPass->top,
                                                     (float)view.renderPass->near, (float)view.renderPass->far);

        batch_->begin();
        {
            float mx[16];
            GLES20FixedPipeline::getInstance()->readMatrix(GLES20FixedPipeline::MatrixMode::MM_GL_PROJECTION, mx);
            this->batch_->setMatrix(GL_PROJECTION, mx);
            GLES20FixedPipeline::getInstance()->readMatrix(GLES20FixedPipeline::MatrixMode::MM_GL_MODELVIEW, mx);
            this->batch_->setMatrix(GL_MODELVIEW, mx);
        }

        std::vector<atakmap::math::Rectangle<double>> label_placements;

        if (draw_version_ != view.drawVersion) {
            draw_version_ = view.drawVersion;
            replace_labels_ = true;
        }
        if (always_render_idx_ != NO_ID) {
            GLLabel& label = labels_[always_render_idx_];

            const Feature::Geometry2* geometry = label.getGeometry();
            if (geometry != nullptr) {
                label.validateProjectedLocation(view);
                GLText2 *gltext = label.gltext_;
                if (!gltext)
                    gltext = defaultText;
                label.place(view, *gltext, label_placements);
                label.batch(view, *gltext, *(batch_.get()));
                label_placements.push_back(label.labelRect);
            }
        }

        draw(view, Priority::TEP_High, label_placements);
        draw(view, Priority::TEP_Standard, label_placements);
        draw(view, Priority::TEP_Low, label_placements);

        batch_->end();

        GLES20FixedPipeline::getInstance()->glMatrixMode(GLES20FixedPipeline::MatrixMode::MM_GL_PROJECTION);
        GLES20FixedPipeline::getInstance()->glPopMatrix();

        GLES20FixedPipeline::getInstance()->glMatrixMode(GLES20FixedPipeline::MatrixMode::MM_GL_MODELVIEW);
        GLES20FixedPipeline::getInstance()->glPopMatrix();
    } catch (...) {
        // ignored
    }

    replace_labels_ = false;
}

void GLLabelManager::draw(const GLGlobeBase& view, const Priority priority,
                          std::vector<atakmap::math::Rectangle<double>>& label_placements) NOTHROWS {
    auto ids_it = label_priorities_.find(priority);
    if (ids_it == label_priorities_.end()) return;
    auto ids = ids_it->second;

    for (auto it = ids.begin(); it != ids.end(); it++) {
        const uint32_t label_id = *it;
        if (label_id == always_render_idx_) continue;

        auto label_it = labels_.find(label_id);
        if (label_it == labels_.end()) continue;

        GLLabel& label = label_it->second;

        if (label.text_.empty()) continue;
        if (!label.shouldRenderAtResolution(view.renderPass->drawMapResolution)) continue;

        label.validateProjectedLocation(view);

        double range;
        Vector2_length(&range, Point2<double>(label.pos_projected_.x - view.renderPass->scene.camera.location.x,
                                              label.pos_projected_.y - view.renderPass->scene.camera.location.y,
                                              label.pos_projected_.z - view.renderPass->scene.camera.location.z));

        // avoid any points on other side of earth
        if (range > atakmap::core::Ellipsoid::WGS84.semiMajorAxis) continue;

        Point2<double> xyz;
        view.renderPass->scene.forwardTransform.transform(&xyz, label.pos_projected_);
        // confirm location is in view
        if (!atakmap::math::Rectangle<double>::contains(view.renderPass->left, view.renderPass->bottom, view.renderPass->right, view.renderPass->top, xyz.x, xyz.y)) continue;

        GLText2* gltext = label.gltext_;
        if (!gltext) gltext = defaultText;
        if (replace_labels_) {
            label.place(view, *gltext, label_placements);
        }
        if (label.canDraw) {
            label.batch(view, *gltext, *(batch_.get()));
            label_placements.push_back(label.labelRect);
        }
    }
}


void GLLabelManager::release() NOTHROWS {}

int GLLabelManager::getRenderPass() NOTHROWS { return GLMapView2::Sprites; }

void GLLabelManager::start() NOTHROWS {}

void GLLabelManager::stop() NOTHROWS {
    Lock lock(mutex_);

    labels_.clear();
}

GLText2* GLLabelManager::getDefaultText() NOTHROWS {
    if (defaultText == nullptr) {
        TAKErr code;
        TextFormat2Ptr fmt(nullptr, nullptr);
        if (!defaultFontSize) {
            Port::String opt;
            code = ConfigOptions_getOption(opt, "default-font-size");
            if (code == TE_Ok)
                defaultFontSize = (float)atof(opt);
            else
                defaultFontSize = 14.0f;
        }
        code = TextFormat2_createDefaultSystemTextFormat(fmt, defaultFontSize);
        if (code == TE_Ok) {
            std::shared_ptr<TextFormat2> sharedFmt = std::move(fmt);
            defaultText = GLText2_intern(sharedFmt);
        }
    }
    return defaultText;
}
