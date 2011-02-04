/*
 * $Revision: 545 $ $Date: 2010-10-20 18:58:01 -0700 (Wed, 20 Oct 2010) $
 *
 * Copyright by Astos Solutions GmbH, Germany
 *
 * this file is published under the Astos Solutions Free Public License
 * For details on copyright and terms of use see 
 * http://www.astos.de/Astos_Solutions_Free_Public_License.html
 */

#include "UniverseRenderer.h"
#include "RenderContext.h"
#include "Observer.h"
#include "Geometry.h"
#include "Debug.h"
#include "BoundingSphere.h"
#include "PlanarProjection.h"
#include "Visualizer.h"
#include "OGLHeaders.h"
#include "Framebuffer.h"
#include "CubeMapFramebuffer.h"
#include "glhelp/GLFramebuffer.h"
#include "Units.h"
#include <Eigen/Geometry>
#include <algorithm>

using namespace vesta;
using namespace Eigen;
using namespace std;


// Renderer debug settings
#define DEBUG_SHADOW_MAP      0
#define DEBUG_OMNI_SHADOW_MAP 0
#define DEBUG_DEPTH_SPANS     0


const float UniverseRenderer::MinimumNearDistance = 0.00001f;  // 1 centimeter
const float UniverseRenderer::MaximumFarDistance = 1.0e12; // one trillion km (~6700 AU)

static const float MinimumNearPlaneDistance = 0.00001f;  // 1 centimeter
static const float MaximumFarPlaneDistance = 1.0e12; // one trillion km (~6700 AU)
static const float MinimumNearFarRatio = 0.001f;
static const float PreferredNearFarRatio = 0.002f;

// Camera rotations used for drawing to the faces of a cube map
static const Quaterniond Z180 = Quaterniond(AngleAxisd(toRadians(180.0), Vector3d::UnitZ()));
static const Quaterniond CubeFaceCameraRotations[6] =
{
    Quaterniond(AngleAxisd(toRadians(-90.0), Vector3d::UnitY())) * Z180,
    Quaterniond(AngleAxisd(toRadians( 90.0), Vector3d::UnitY())) * Z180,
    Quaterniond(AngleAxisd(toRadians( 90.0), Vector3d::UnitX())) * Z180,
    Quaterniond(AngleAxisd(toRadians(-90.0), Vector3d::UnitX())) * Z180,
    Quaterniond(AngleAxisd(toRadians(  0.0), Vector3d::UnitY())) * Z180,
    Quaterniond(AngleAxisd(toRadians(180.0), Vector3d::UnitY())) * Z180,
};


/** Construct a new UniverseRenderer. The renderer may not be used for drawing
  * until its initializeGraphics method has been called. Initialization is not
  * performed in the constructor: a UniverseRenderer can be created at any time,
  * but the graphics state can only be initialized once an OpenGL context is
  * available.
  */
UniverseRenderer::UniverseRenderer() :
    m_renderContext(NULL),
    m_universe(NULL),
    m_currentTime(0.0),
    m_shadowsEnabled(false),
    m_visualizersEnabled(true),
    m_skyLayersEnabled(true),
    m_renderViewport(1, 1)
{
}


UniverseRenderer::~UniverseRenderer()
{
    delete m_renderContext;
}


/** Return true if shadows are supported for this renderer. In order to support shadows,
 *  the OpenGL implementation must support both shaders and framebuffer objects.
 */
bool
UniverseRenderer::shadowsSupported() const
{
    return Framebuffer::supported() && m_renderContext && m_renderContext->shaderCapability() != RenderContext::FixedFunction;
}


/** Return true if omnidirectinoal shadows are supported for this renderer. In order to
 *  support shadows the OpenGL implementation must support shaders, framebuffer objects,
 *  cube maps, and floating point textures.
 */
bool
UniverseRenderer::omniShadowsSupported() const
{
    return shadowsSupported() && GLEW_ARB_texture_cube_map == GL_TRUE && GLEW_ARB_texture_rg;
}


/** Enable or disable the drawing of shadows.
  */
void
UniverseRenderer::setShadowsEnabled(bool enable)
{
    if (!m_shadowMaps[0].isNull() && m_shadowMaps[0]->isValid())
    {
        m_shadowsEnabled = enable;
    }
}


/** Enable or disable the drawing of visualizers.
  */
void
UniverseRenderer::setVisualizersEnabled(bool enable)
{
    m_visualizersEnabled = enable;
}


/** Enable or disable the drawing of sky layers. Layers may
  * also be shown or hidden individually by calling setVisibility()
  * on the layer. In order for a layer to be drawn, sky layers
  * must be enabled in the renderer and the visibility of the
  * layer must be set to true.
  */
void
UniverseRenderer::setSkyLayersEnabled(bool enable)
{
    m_skyLayersEnabled = enable;
}


/** Initialize all graphics resources. This method must only be called once OpenGL has
  * been initialized and a GL context has been set. The renderer cannot be used for
  * drawing until initializeGraphics is called successfully.
  *
  * \return true if the graphics system was successfully initialized, false otherwise
  */
bool
UniverseRenderer::initializeGraphics()
{
    if (m_renderContext)
    {
        // The renderer has already been successfully initialized.
        return true;
    }

    m_renderContext = RenderContext::Create();

    return m_renderContext != NULL;
}


/** Initialize shadows for this renderer.
  *
  * @param shadowMapSize dimension of the square shadow map. A higher value will produce
  * better shadows but consume more memory. A smaller map may be allocated if the requested
  * size is larger than the maximum texture size supported by hardware
  *
  * @param shadowMapCount number of shadow maps to allocate. The number of shadows cast on
  *                       any one body is limited by this value.
  *
  * \return true if the shadow map resources were successfully created
  */
bool
UniverseRenderer::initializeShadowMaps(unsigned int shadowMapSize, unsigned int shadowMapCount)
{
    if (!m_renderContext)
    {
        VESTA_WARNING("UniverseRenderer::initializeShadowMaps() called before initializeGraphics()");
        return false;
    }

    if (!shadowsSupported())
    {
        VESTA_LOG("Shadows not supported by graphic hardware and/or drivers.");
        return false;
    }

    if (shadowMapCount > MaxShadowMaps)
    {
        VESTA_LOG("Too many shadow maps requested. Using limit of %d", MaxShadowMaps);
        shadowMapCount = MaxShadowMaps;
    }

    // Constrain the shadow map size to the maximum size permitted by the hardware
    GLint maxTexSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSize);
    shadowMapSize = min((unsigned int) maxTexSize, shadowMapSize);

    m_shadowsEnabled = false;
    m_shadowMaps.clear();

    for (unsigned int i = 0; i < shadowMapCount; ++i)
    {
        counted_ptr<Framebuffer> shadowMap(Framebuffer::CreateDepthOnlyFramebuffer(shadowMapSize, shadowMapSize));
        if (shadowMap.isNull())
        {
            VESTA_LOG("Failed to create shadow buffer %d. Shadows not enabled.", i);
            m_shadowMaps.clear();
            return false;
        }
        m_shadowMaps.push_back(shadowMap);
    }

    VESTA_LOG("Created %d %dx%d shadow buffer(s) for UniverseRenderer.", shadowMapCount, shadowMapSize, shadowMapSize);

    return true;
}


/** Initialize omnidirectional shadow map resources for this renderer.
  *
  * \param shadowMapSize dimension of the shadow map. A higher value will produce
  * better shadows but consume more memory. A smaller map may be allocated if the requested
  * size is larger than the maximum texture size supported by hardware
  *
  * \param shadowMapCount number of shadow maps to allocate. The number of shadows cast on
  *                       any one body is limited by this value.
  *
  * \return true if the shadow map resources were successfully created
  */
bool
UniverseRenderer::initializeOmniShadowMaps(unsigned int shadowMapSize, unsigned int shadowMapCount)
{
    if (!m_renderContext)
    {
        VESTA_WARNING("UniverseRenderer::initializeOmniShadowMaps() called before initializeGraphics()");
        return false;
    }

    if (!omniShadowsSupported())
    {
        VESTA_LOG("Omnidirectional shadows not supported by graphic hardware and/or drivers.");
        return false;
    }

    if (shadowMapCount > MaxOmniShadowMaps)
    {
        VESTA_LOG("Too many shadow maps requested. Using limit of %d", MaxShadowMaps);
        shadowMapCount = MaxShadowMaps;
    }

    // Constrain the shadow map size to the maximum size permitted by the hardware
    GLint maxTexSize = 0;
    glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE_ARB, &maxTexSize);
    shadowMapSize = min((unsigned int) maxTexSize, shadowMapSize);

    m_omniShadowMaps.clear();

    // Omnidirectional shadows are implemented as cube maps with the camera to fragment distance
    // stored in the red channel. We require 32-bit floating point precision for storing distances.
    for (unsigned int i = 0; i < shadowMapCount; ++i)
    {
        counted_ptr<CubeMapFramebuffer> shadowMap(CubeMapFramebuffer::CreateCubicReflectionMap(shadowMapSize, TextureMap::R32F));
        if (shadowMap.isNull())
        {
            VESTA_LOG("Failed to create omni shadow buffer %d. Omni shadows not enabled.", i);
            m_omniShadowMaps.clear();
            return false;
        }

        m_omniShadowMaps.push_back(shadowMap);
    }

    VESTA_LOG("Created %d %dx%d cube map shadow buffer(s) for UniverseRenderer.", shadowMapCount, shadowMapSize, shadowMapSize);

    return true;
}


/** Set up the renderer to draw one or more views at the specified time.
  * The renderer can perform optimizations that improve performance when
  * multiple views are rendered within the same view set. These optimizations
  * assume that no changes are made to objects in the universe in between
  * beginViewSet / endViewSet. If objects are being changed between calls
  * to renderView(), the calls should appear in different view sets.
  *
  * \param universe the universe to be rendered
  * \param tsec simulation time in seconds since J2000 TDB
  *
  * \return a status code indicating whether the view set was
  *         set up successfully. If there were no problems, this
  *         method returns RendererOk. Otherwise, the beginViewSet()
  *         will return:
  *         \list
  *         \li RendererUninitialized - if called before initializeGraphics()
  *         \li RendererBadParameter - if universe is null
  *         \li RendererViewSetAlreadyStarted - if called after a previous
  *             beginViewSet() call but before endViewSet()
  */
UniverseRenderer::RenderStatus
UniverseRenderer::beginViewSet(const Universe* universe, double tsec)
{
    if (!m_renderContext)
    {
        return RendererUninitialized;
    }

    if (universe == NULL)
    {
        return RendererBadParameter;
    }

    if (m_universe)
    {
        return RenderViewSetAlreadyStarted;
    }

    m_universe = universe;
    m_currentTime = tsec;

    // Build the light source list
    // TODO: maintain a bounding sphere hierarchy in order to avoid having to do a linear
    // traversal of all objects.
    m_lightSources.clear();

    // Add a light source for the Sun
    // TODO: Consider whether it might be good to *not* set this automatically
    LightSourceItem sunItem;
    sunItem.lightSource = 0;
    sunItem.position = Vector3d::Zero();
    m_lightSources.push_back(sunItem);

    const vector<Entity*>& entities = m_universe->entities();
    for (vector<Entity*>::const_iterator iter = entities.begin(); iter != entities.end(); ++iter)
    {
        const Entity* entity = *iter;
        const LightSource* light = entity->lightSource();

        if (light && entity->isVisible(m_currentTime))
        {
            Vector3d position = entity->position(m_currentTime);

            LightSourceItem lsi;
            lsi.lightSource = light;
            lsi.position = position;
            m_lightSources.push_back(lsi);
        }
    }

    return RenderOk;
}


/** Finish the current view set.
  *
  * \return the render status, which will be RenderNoViewSet if endViewSet() is
  *         called before beginViewSet(). Otherwise, endViewSet() returns RenderOk.
  */
UniverseRenderer::RenderStatus
UniverseRenderer::endViewSet()
{
    if (!m_universe)
    {
        return RenderNoViewSet;
    }

    m_universe = NULL;

    return RenderOk;
}


static bool
visibleItemPredicate(const vesta::UniverseRenderer::VisibleItem& item0,
                     const vesta::UniverseRenderer::VisibleItem& item1)
{
    return item0.farDistance < item1.farDistance;
}


#if DEBUG_SHADOW_MAP
// Debugging code for shadows
static void
showShadowMap(Framebuffer* shadowMap,
              float quadSize,
              float viewportWidth, float viewportHeight)
{
    if (shadowMap->isValid())
    {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluOrtho2D(0, viewportWidth, 0, viewportHeight);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glDisable(GL_LIGHTING);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glDisable(GL_DEPTH_TEST);

        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, shadowMap->depthTexHandle());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f);
        glVertex2f(0.0f, 0.0f);
        glTexCoord2f(1.0f, 0.0f);
        glVertex2f(quadSize, 0.0f);
        glTexCoord2f(1.0f, 1.0f);
        glVertex2f(quadSize, quadSize);
        glTexCoord2f(0.0f, 1.0f);
        glVertex2f(0.0f, quadSize);
        glEnd();

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
    }
}
#endif // DEBUG_SHADOW_MAP


#if DEBUG_OMNI_SHADOW_MAP
// Debugging code for omnidirectional shadows
static void
showOmniShadowMap(CubeMapFramebuffer* shadowMap,
                  float quadSize,
                  float viewportWidth, float viewportHeight)
{
    if (shadowMap->colorTexture())
    {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluOrtho2D(0, viewportWidth, 0, viewportHeight);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glDisable(GL_LIGHTING);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glDisable(GL_DEPTH_TEST);

        glEnable(GL_TEXTURE_CUBE_MAP);
        glBindTexture(GL_TEXTURE_CUBE_MAP, shadowMap->colorTexture()->id());

        float halfAngle = toRadians(60.0f);
        glBegin(GL_QUADS);
        glTexCoord3f(cos(-halfAngle), sin(-halfAngle), -1.0f);
        glVertex2f(0.0f, 0.0f);
        glTexCoord3f(cos(halfAngle), sin(-halfAngle), 1.0f);
        glVertex2f(quadSize, 0.0f);
        glTexCoord3f(cos(halfAngle), sin(halfAngle), 1.0f);
        glVertex2f(quadSize, quadSize);
        glTexCoord3f(cos(-halfAngle), sin(halfAngle), -1.0f);
        glVertex2f(0.0f, quadSize);
        glEnd();

        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        glDisable(GL_TEXTURE_CUBE_MAP);
    }
}
#endif // DEBUG_OMNI_SHADOW_MAP


static bool skyLayerOrderPredicate(const SkyLayer* layer0, const SkyLayer* layer1)
{
    return layer0->drawOrder() < layer1->drawOrder();
}


/** Render visible bodies in the universe using the specified camera position,
  * orientation, and projection.
  *
  * @param lighting information about lights and shadows that could affect objects in view
  * @param cameraPosition the camera position
  * @param cameraOrientation the camera orientation
  * @param projection the camera projection
  * @param viewport rectangular region of the rendering surface to draw into
  * @param renderSurface target framebuffer; the default value of NULL means that the default back buffer will be used.
  */
UniverseRenderer::RenderStatus
UniverseRenderer::renderView(const LightingEnvironment* lighting,
                             const Vector3d& cameraPosition,
                             const Quaterniond& cameraOrientation,
                             const PlanarProjection& projection,
                             const Viewport& viewport,
                             Framebuffer* renderSurface)
{
    if (!m_universe)
    {
        return RenderNoViewSet;
    }

    // Save the viewport and render surface so that they can be reset after
    // shadow and reflection rendering.
    m_renderSurface = renderSurface;
    m_renderViewport = viewport;

    // Save the current color mask
    GLboolean mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    for (unsigned int i = 0; i < 4; ++i)
    {
        m_renderColorMask[i] = mask[i] == GL_TRUE;
    }

    glViewport(viewport.x(), viewport.y(), viewport.width(), viewport.height());

    Matrix3f toCameraSpace = cameraOrientation.conjugate().cast<float>().toRotationMatrix();
    float aspectRatio = viewport.aspectRatio();
    float fieldOfView = projection.fovY();

    // Reverse the vertex winding order if we have a left-handed projection matrix
    // (because all geometry assumes a right-handed projection.)
    if (projection.chirality() == PlanarProjection::LeftHanded)
    {
        glFrontFace(GL_CW);
    }

    glShadeModel(GL_SMOOTH);
    glEnable(GL_CULL_FACE);

    m_renderContext->setCameraOrientation(cameraOrientation.cast<float>());
    m_renderContext->setPixelSize((float) (2 * tan(fieldOfView / 2.0) / viewport.height()));
    m_renderContext->setViewportSize(viewport.width(), viewport.height());

    m_renderContext->pushModelView();
    m_renderContext->rotateModelView(cameraOrientation.conjugate().cast<float>());

    // Draw sky layers grids
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);

    m_renderContext->setProjection(projection.slice(0.1f, 1.0f));

    if (m_skyLayersEnabled)
    {
        vector<SkyLayer*> visibleLayers;
        const Universe::SkyLayerTable* skyLayers = m_universe->layers();
        for (Universe::SkyLayerTable::const_iterator iter = skyLayers->begin(); iter != skyLayers->end(); ++iter)
        {
            SkyLayer* layer = iter->second.ptr();
            if (layer && layer->isVisible())
            {
                visibleLayers.push_back(layer);
            }

            sort(visibleLayers.begin(), visibleLayers.end(), skyLayerOrderPredicate);
        }

        for (vector<SkyLayer*>::const_iterator iter = visibleLayers.begin(); iter != visibleLayers.end(); ++iter)
        {
            glDisable(GL_LIGHTING);
            (*iter)->render(*m_renderContext);
        }
    }

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    // Fixed function state setup
    glEnable(GL_NORMALIZE);
    glEnable(GL_LIGHTING);

    m_renderContext->setActiveLightCount(1);
    m_renderContext->setAmbientLight(m_ambientLight);

    m_viewFrustum = projection.frustum();

    // This adjustment factor will ensure that the view frustum near plane
    // doesn't intersect the geometry of a body.
    float nearPlaneFovAdjustment = (float) (cos(fieldOfView / 2.0) / sqrt(1.0 + aspectRatio * aspectRatio));

    const vector<Entity*>& entities = m_universe->entities();

    m_visibleItems.clear();
    m_splittableItems.clear();

    m_lighting = lighting;

    buildVisibleLightSourceList(cameraPosition);

    // Simply scan through all entities in the universe. For much better performance with
    // many entities, we should maintain a bounding sphere hierarchy.
    for (vector<Entity*>::const_iterator iter = entities.begin(); iter != entities.end(); ++iter)
    {
        const Entity* entity = *iter;

        if (entity->isVisible(m_currentTime))
        {
            Vector3d position = entity->position(m_currentTime);

            // Calculate the difference at double precision, then convert to single
            // precision for the rest of the work.
            Vector3d cameraRelativePosition = (position - cameraPosition);

            // Cull objects based on size. If an object is less than one pixel in size,
            // we don't draw its geometry. Visualizers have sizes that may be unrelated
            // to the size of the object, so we don't cull them.
            // TODO: Add a method to the visualizer class that specifies whether the size
            // culling test (i.e. if the visualizer geometry has a fixed size on screen--such
            // as a label--then it shouldn't be culled.)
            bool sizeCull = false;
            if (entity->geometry())
            {
                float projectedSize = (entity->geometry()->boundingSphereRadius() / float(cameraRelativePosition.norm())) / m_renderContext->pixelSize();
                sizeCull = projectedSize < 0.5f;
            }
            else
            {
                // Objects without geometry are always culled.
                sizeCull = true;
            }

            // We need the camera space position of the object in order to depth
            // sort the objects.
            Vector3f cameraSpacePosition = toCameraSpace * cameraRelativePosition.cast<float>();

            if (!sizeCull)
            {
                addVisibleItem(entity, entity->geometry(),
                               position, cameraRelativePosition, cameraSpacePosition,
                               entity->orientation(m_currentTime).cast<float>(),
                               nearPlaneFovAdjustment);
            }

            if (entity->hasVisualizers() && m_visualizersEnabled)
            {
                for (Entity::VisualizerTable::const_iterator iter = entity->visualizers()->begin();
                     iter != entity->visualizers()->end(); ++iter)
                {
                    const Visualizer* visualizer = iter->second.ptr();
                    if (visualizer->isVisible())
                    {
                        Vector3d adjustedPosition = cameraRelativePosition;
                        Vector3f adjustedCameraSpacePosition = cameraSpacePosition;

                        if (visualizer->depthAdjustment() == Visualizer::AdjustToFront)
                        {
                            // Adjust the position of the visualizer so that it is drawn in
                            // front of the object to which it is attached.
                            if (entity->geometry())
                            {
                                float z = -cameraSpacePosition.z() - entity->geometry()->boundingSphereRadius();
                                float f = z / -cameraSpacePosition.z();
                                adjustedPosition *= f;
                                adjustedCameraSpacePosition *= f;
                            }
                        }

                        addVisibleItem(entity, visualizer->geometry(),
                                       position, adjustedPosition, adjustedCameraSpacePosition,
                                       visualizer->orientation(entity, m_currentTime).cast<float>(),
                                       nearPlaneFovAdjustment);
                    }
                }
            }
        }
    }

    // Depth sort all visible items
    sort(m_visibleItems.begin(), m_visibleItems.end(), visibleItemPredicate);
    sort(m_splittableItems.begin(), m_splittableItems.end(), visibleItemPredicate);

    splitDepthBuffer();
    coalesceDepthBuffer();

    // If there is splittable geometry, we need to add extra depth spans
    // at the front and back, otherwise it may be clipped.
    if (!m_splittableItems.empty())
    {
        // Use a different near/far ratio for these extra spans
        const float MaxFarNearRatio = 10000.0f;

        float furthestDistance = min(m_splittableItems.front().farDistance, projection.farDistance());

        // Handle the case when the only visible geometry is splittable. This can happen
        // in solar system views where just the planet orbits are visible. The only thing
        // that we need to do is add the furthest span.
        if (m_depthBufferSpans.empty())
        {
            DepthBufferSpan back;
            back.backItemIndex = 0;
            back.itemCount = 0;
            back.farDistance = projection.farDistance();
            back.nearDistance = max(projection.nearDistance(), back.farDistance / MaxFarNearRatio);
            m_mergedDepthBufferSpans.push_back(back);
        }
        else if (furthestDistance > m_mergedDepthBufferSpans.front().farDistance)
        {
            DepthBufferSpan back;
            back.backItemIndex = 0;
            back.itemCount = 0;
            back.farDistance = furthestDistance;
            back.nearDistance = m_mergedDepthBufferSpans.front().farDistance;
            m_mergedDepthBufferSpans.insert(m_mergedDepthBufferSpans.begin(), back);
        }

        while (m_mergedDepthBufferSpans.back().nearDistance > projection.nearDistance())
        {
            // Some potentially confusing naming here: spans are stored in
            // reverse order, so that the foreground span is actually the
            // *last* one in the list.
            DepthBufferSpan front;
            front.backItemIndex = 0;
            front.itemCount = 0;
            front.farDistance = m_mergedDepthBufferSpans.back().nearDistance;
            front.nearDistance = std::max(projection.nearDistance(), front.farDistance / MaxFarNearRatio);
            m_mergedDepthBufferSpans.push_back(front);
        }

        DepthBufferSpan back;
        back.backItemIndex = 0;
        back.itemCount = 0;
        back.nearDistance = m_mergedDepthBufferSpans.front().farDistance;
        back.farDistance = back.nearDistance * MaxFarNearRatio;
        m_mergedDepthBufferSpans.insert(m_mergedDepthBufferSpans.begin(), back);
    }

#if DEBUG_DEPTH_SPANS
    // cerr << "split: " << m_depthBufferSpans.size() << ", merged: " << m_mergedDepthBufferSpans.size() << endl;
    cerr << "spans: ";
    for (unsigned int i = 0; i < m_depthBufferSpans.size(); ++i)
    {
        cerr << "( " << m_depthBufferSpans[i].nearDistance << ", " << m_depthBufferSpans[i].farDistance << " ) ";
    }
    cerr << endl;

    cerr << "merged: ";
    for (unsigned int i = 0; i < m_mergedDepthBufferSpans.size(); ++i)
    {
        cerr << "( " << m_mergedDepthBufferSpans[i].nearDistance << ", " << m_mergedDepthBufferSpans[i].farDistance << " ) ";
    }
    cerr << endl;
#endif // DEBUG_DEPTH_SPANS

    // Draw depth buffer spans from back to front
    unsigned int spanIndex = m_mergedDepthBufferSpans.size() - 1;
    float spanRange = 1.0f;
    if (!m_mergedDepthBufferSpans.empty())
    {
        spanRange /= (float) m_mergedDepthBufferSpans.size();
    }

    for (vector<DepthBufferSpan>::const_iterator iter = m_mergedDepthBufferSpans.begin();
         iter != m_mergedDepthBufferSpans.end(); ++iter, --spanIndex)
    {
        setDepthRange(spanIndex * spanRange, (spanIndex + 1) * spanRange);
        renderDepthBufferSpan(*iter, projection);
    }
    setDepthRange(0.0f, 1.0f);

    m_renderContext->popModelView();

    m_renderContext->unbindShader();

    // Reset the front face
    glFrontFace(GL_CCW);

#if DEBUG_SHADOW_MAP
    if (m_shadowsEnabled && m_shadowMap.isValid())
    {
        showShadowMap(m_shadowMap.ptr(), 320.0f, viewport.width(), viewport.height());
    }
#endif

#if DEBUG_OMNI_SHADOW_MAP
    if (m_shadowsEnabled && m_omniShadowMap.isValid())
    {
        showOmniShadowMap(m_omniShadowMap.ptr(), 320.0f, viewport.width(), viewport.height());
    }
#endif

    // Don't hold on to the lighting environment pointer
    m_lighting = NULL;

    return RenderOk;
}


/** Render visible bodies in the universe from the point of view of the
  * specified observer.
  *
  * @param information about lights and shadows that could affect objects in view
  * @param observer the observer.
  * @param fieldOfView the horizontal field of view in radians
  * @param viewport rectangular region of the rendering surface to draw into
  * @param renderSurface target framebuffer; the default value of NULL means that the default back buffer will be used.
  */
UniverseRenderer::RenderStatus
UniverseRenderer::renderView(const LightingEnvironment* lighting,
                             const Observer* observer,
                             double fieldOfView,
                             const Viewport& viewport,
                             Framebuffer* renderSurface)
{
    return renderView(lighting,
                      observer->absolutePosition(m_currentTime),
                      observer->absoluteOrientation(m_currentTime),
                      PlanarProjection::CreatePerspective(fieldOfView, viewport.aspectRatio(), MinimumNearPlaneDistance, MaximumFarPlaneDistance),
                      viewport,
                      renderSurface);
}


/** Render visible bodies in the universe from the point of view of the
  * specified observer. This method is just a shortcut for the renderView
  * method that accepts a render surface and viewport parameter.
  *
  * @param observer the observer.
  * @param fieldOfView the horizontal field of view in radians
  * @param viewportWidth the width of the viewport in pixels
  * @param viewportHeight the height of the viewport in pixels
  */
UniverseRenderer::RenderStatus
UniverseRenderer::renderView(const Observer* observer,
                             double fieldOfView,
                             int viewportWidth,
                             int viewportHeight)
{
    return renderView(NULL, observer, fieldOfView, Viewport(viewportWidth, viewportHeight), NULL);
}


// Predicate used for sorting light sources so that shadow casters are first
static bool lightCastsShadowsPredicate(const UniverseRenderer::VisibleLightSourceItem& light0,
                                       const UniverseRenderer::VisibleLightSourceItem& light1)
{
    // Handle the special case for the Sun, which casts shadows but has a NULL lightSource.
    int shadow0 = !light0.lightSource || light0.lightSource->isShadowCaster() ? 1 : 0;
    int shadow1 = !light1.lightSource || light1.lightSource->isShadowCaster() ? 1 : 0;
    return shadow0 > shadow1;
}


// Private method to create the visible light source list from the main light
// source list. Only light sources which interact with objects in the view
// frustum will appear in the visible light list.
void
UniverseRenderer::buildVisibleLightSourceList(const Vector3d& cameraPosition)
{
    Matrix3f toCameraSpace = m_renderContext->cameraOrientation().conjugate().toRotationMatrix();

    // Create the list of visible light sources. We filter the list of all light sources
    // and only keep the ones that interact with objects in the view frustum.
    m_visibleLightSources.clear();
    for (vector<LightSourceItem>::const_iterator iter = m_lightSources.begin(); iter != m_lightSources.end(); ++iter)
    {
        const LightSourceItem& lsi = *iter;
        Vector3d cameraRelativePosition = lsi.position - cameraPosition;

        bool cull = false;
        if (lsi.lightSource != NULL)
        {
            float projectedSize = (lsi.lightSource->range() / float(cameraRelativePosition.norm())) / m_renderContext->pixelSize();
            if (projectedSize < 1.0f)
            {
                // Light might be in the view frustum, but it affects a region that occupies less than
                // a pixel on screen.
                cull = true;
            }
            else
            {
                // Check whether the light lies outside the view frustum. We can disregard it if it does.
                Vector3f cameraSpacePosition = toCameraSpace * cameraRelativePosition.cast<float>();
                if (!m_viewFrustum.intersects(BoundingSphere<float>(cameraSpacePosition, lsi.lightSource->range())))
                {
                    cull = true;
                }
            }
        }
        else
        {
            // Handle the Sun specially--it is never culled.
        }

        if (!cull)
        {
            VisibleLightSourceItem visibleLight;
            visibleLight.lightSource = lsi.lightSource;
            visibleLight.position = lsi.position;
            visibleLight.cameraRelativePosition = cameraRelativePosition;
            m_visibleLightSources.push_back(visibleLight);
        }
    }

    // Sort the light sources so that the shadow casters appear first in the visible
    // light sources list.
    sort(m_visibleLightSources.begin(), m_visibleLightSources.end(), lightCastsShadowsPredicate);
}


void
UniverseRenderer::setDepthRange(float front, float back)
{
    m_depthRangeFront = front;
    m_depthRangeBack = back;
    glDepthRange(front, back);
}


void
UniverseRenderer::addVisibleItem(const Entity* entity,
                                 const Geometry* geometry,
                                 const Vector3d& position,
                                 const Vector3d& cameraRelativePosition,
                                 const Vector3f& cameraSpacePosition,
                                 const Quaternionf& orientation,
                                 float nearAdjust)
{
    // Compute the signed distance from the camera plane to the most
    // distant part of the entity. A distance < 0 indicates that the
    // entity lies completely behind the camera.
    float boundingRadius = geometry->boundingSphereRadius();
    float farDistance = -cameraSpacePosition.z() + boundingRadius;

    // Calculate a near distance that's as far from the camera as possible.
    float nearDistance = geometry->nearPlaneDistance(orientation.conjugate() * -cameraRelativePosition.cast<float>());

    // Generally, the near distance for an individual object will never be less
    // than MinimumNearFarRatio times the bounding diameter. Exceptions are things
    // like trajectories, which should never be clipped by the near plane. This
    // is handled by marking trajectories as splittable, so that they will be
    // drawn into multiple depth buffer spans when necessary.
    switch (geometry->clippingPolicy())
    {
    case Geometry::PreserveDepthPrecision:
        nearDistance = std::max(nearDistance, boundingRadius * MinimumNearFarRatio * 2.0f);
        break;

    case Geometry::PreventClipping:
    case Geometry::SplitToPreventClipping:
        nearDistance = std::max(nearDistance, MinimumNearPlaneDistance);
        break;
    }

    // ...but make sure that the near plane of the view frustum doesn't
    // intersect the object's geometry. Note that if nearDistance is greater
    // farDistance, it means that the object lies outside the view frustum.
    nearDistance *= nearAdjust;

    bool intersectsFrustum = m_viewFrustum.intersects(BoundingSphere<float>(cameraSpacePosition, boundingRadius));

    // Objects that lie outside the frustum and don't cast shadows don't contribute to the
    // final scene. They'll be culled eventually, but we can take of them early here. However,
    // enabling this test causes problems right now with disappearing visualizers when ordinary
    // objects are in view.
    /*
    if (!intersectsFrustum && !geometry->isShadowCaster())
    {
        return;
    }
    */

    // Add entities in front of the camera to the list of visible items
    if (farDistance > 0 && nearDistance < farDistance)
    {
        VisibleItem visibleItem;
        visibleItem.entity = entity;
        visibleItem.geometry = geometry;
        visibleItem.position = position;
        visibleItem.cameraRelativePosition = cameraRelativePosition;
        visibleItem.orientation = orientation;
        visibleItem.boundingRadius = boundingRadius;
        visibleItem.nearDistance = nearDistance;
        visibleItem.farDistance = farDistance;
        visibleItem.outsideFrustum = !intersectsFrustum;

        if (geometry->clippingPolicy() == Geometry::SplitToPreventClipping)
        {
            m_splittableItems.push_back(visibleItem);
        }
        else
        {
            m_visibleItems.push_back(visibleItem);
        }
    }
}


/** Render six views into the faces of a cube map from the specified position. The views are pointed
  * along the universal coordinate system axes, though this can be modified by passing something
  * other than identity for the rotation.
  *
  * Reflection maps are expected to be in world coordinates. If the cube map is intended to be used
  * for reflections, the rotation should be identity (the default value)
  *
  * In order to avoid obvious visual problems with reflection maps, they should only contain geometry that
  * is 'distant', i.e. at a much farther away than the size of the reflecting geometry. The nearDistance
  * can be set to a value greater than the minimum in order to automatically cull nearby objects.
  *
  * \param lighting the lighting environment for rendering
  * \param position position of the camera
  * \param cubeMap the target cube map framebuffer to draw into
  * \param nearDistance distance to the near clipping plane (defaults to MinimumNearDistance)
  * \param farDistance distance to the far clipping plane (defaults to MaximumFarDistance)
  * \param rotation optional rotation (defaults to identity)
  */
UniverseRenderer::RenderStatus
UniverseRenderer::renderCubeMap(const LightingEnvironment* lighting,
                                const Vector3d& position,
                                CubeMapFramebuffer* cubeMap,
                                double nearDistance,
                                double farDistance,
                                const Quaterniond& rotation)
{
    Viewport viewport(cubeMap->size(), cubeMap->size());
    PlanarProjection cubeFaceProjection = PlanarProjection::CreatePerspectiveLH(float(toRadians(90.0)),
                                                                                1.0f,
                                                                                nearDistance, farDistance);

    for (int face = 0; face < 6; ++face)
    {
        Framebuffer* fb = cubeMap->face(CubeMapFramebuffer::Face(face));
        if (fb)
        {
            fb->bind();
            glDepthMask(GL_TRUE);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            RenderStatus status = renderView(lighting, position, rotation * CubeFaceCameraRotations[face], cubeFaceProjection, viewport, fb);
            if (status != RenderOk)
            {
                Framebuffer::unbind();
                return status;
            }
        }
    }

    Framebuffer::unbind();

    return RenderOk;
}


/** Render six views into the faces of a shadow cube map.
  */
UniverseRenderer::RenderStatus
UniverseRenderer::renderShadowCubeMap(const LightingEnvironment* lighting, const Vector3d& position, CubeMapFramebuffer* cubeMap)
{
    RenderStatus status = RenderOk;

    Viewport viewport(cubeMap->size(), cubeMap->size());
    PlanarProjection cubeFaceProjection = PlanarProjection::CreatePerspectiveLH(float(toRadians(90.0)),
                                                                                1.0f,
                                                                                MinimumNearPlaneDistance, MaximumFarPlaneDistance);

    m_renderContext->setRendererOutput(RenderContext::CameraDistance);

    for (int face = 0; face < 6; ++face)
    {
        Framebuffer* fb = cubeMap->face(CubeMapFramebuffer::Face(face));
        if (fb)
        {
            fb->bind();
            glDepthMask(GL_TRUE);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            status = renderView(lighting, position, CubeFaceCameraRotations[face], cubeFaceProjection, viewport, fb);
            if (status != RenderOk)
            {
                break;
            }
        }
    }

    Framebuffer::unbind();
    m_renderContext->setRendererOutput(RenderContext::FragmentColor);

    return status;
}


// Split the depth buffer up into one or more spans.
void
UniverseRenderer::splitDepthBuffer()
{
    m_depthBufferSpans.clear();

    // Iterate over the visible items from back to front
    for (int i = (int) m_visibleItems.size() - 1; i >= 0; --i)
    {
        const VisibleItem& item = m_visibleItems[i];

        float nearDistance = item.nearDistance;
        if (m_depthBufferSpans.empty())
        {
            DepthBufferSpan span;
            span.backItemIndex = (unsigned int) i;
            span.itemCount = 1;
            span.farDistance = item.farDistance;
            span.nearDistance = nearDistance;

            m_depthBufferSpans.push_back(span);
        }
        else
        {
            DepthBufferSpan& span = m_depthBufferSpans.back();
            bool isDisjoint = item.farDistance < span.nearDistance;

            if (isDisjoint)
            {
                // Item doesn't overlap the current depth buffer span. Create two
                // new spans: one containing item, and one for the empty range in
                // between the new span and the current span.
                DepthBufferSpan emptySpan;
                emptySpan.farDistance = span.nearDistance;
                emptySpan.nearDistance = item.farDistance;
                emptySpan.itemCount = 0;
                emptySpan.backItemIndex = (unsigned int) i;

                // Start a new span
                DepthBufferSpan newSpan;
                newSpan.farDistance = item.farDistance;
                newSpan.nearDistance = nearDistance;
                newSpan.backItemIndex = (unsigned int) i;
                newSpan.itemCount = 1;

                m_depthBufferSpans.push_back(emptySpan);
                m_depthBufferSpans.push_back(newSpan);
            }
            else
            {
                span.itemCount++;
                if (nearDistance < span.nearDistance)
                {
                    span.nearDistance = nearDistance;
                }
            }
        }
    }
}


// Coalesce adjacent depth buffer spans that are of approximately
// the same size. This will prevent over-partitioning of the the
// depth buffer while still preserving a maximum far/near ratio.
void
UniverseRenderer::coalesceDepthBuffer()
{
    m_mergedDepthBufferSpans.clear();

    unsigned int i = 0;
    while (i < m_depthBufferSpans.size())
    {
        float farDistance = m_depthBufferSpans[i].farDistance;
        unsigned int itemCount = m_depthBufferSpans[i].itemCount;

        // Coalesce all spans into a single span that's as large as possible
        // without near/far being less than the preferred near-far ratio. This
        // will reduce the number of depth buffer spans without sacrificing
        // depth buffer precision.
        unsigned int j = i;
        while (j < m_depthBufferSpans.size() - 1)
        {
            if (m_depthBufferSpans[j + 1].nearDistance / farDistance < PreferredNearFarRatio)
            {
                break;
            }

            itemCount += m_depthBufferSpans[j + 1].itemCount;
            ++j;
        }

        DepthBufferSpan span;
        span.farDistance = farDistance;
        span.nearDistance = m_depthBufferSpans[j].nearDistance;
        span.backItemIndex = m_depthBufferSpans[i].backItemIndex;
        span.itemCount = itemCount;

        m_mergedDepthBufferSpans.push_back(span);

        i = j + 1;
    }
}


static void
beginShadowRendering()
{
    // Use depth-only rendering for shadows
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);

    // Reduce 'shadow acne' by rendering the backfaces. This doesn't
    // eliminate the artifacts, but moves them to the unilluminated
    // side of the object, where they're less visible.
    glCullFace(GL_FRONT);
}


static void
beginCubicShadowRendering()
{
    // Render only the red channel
    glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);

    // Reduce 'shadow acne' by rendering the backfaces. This doesn't
    // eliminate the artifacts, but moves them to the unilluminated
    // side of the object, where they're less visible.
    glCullFace(GL_FRONT);
}


// Restore GL state after shadow rendering. Shadow rendering
// affects the render target, color mask, and culling state.
void
finishShadowRendering(Framebuffer* renderSurface, bool colorMask[])
{
    if (renderSurface)
    {
        renderSurface->bind();
    }
    else
    {
        Framebuffer::unbind();
    }

    glColorMask(colorMask[0] ? GL_TRUE : GL_FALSE,
                colorMask[1] ? GL_TRUE : GL_FALSE,
                colorMask[2] ? GL_TRUE : GL_FALSE,
                colorMask[3] ? GL_TRUE : GL_FALSE);

    glCullFace(GL_BACK);
}



// Render all of the items in a depth buffer span
void UniverseRenderer::renderDepthBufferSpan(const DepthBufferSpan& span, const PlanarProjection& projection)
{
    if (span.itemCount == 0 && m_splittableItems.empty())
    {
        return;
    }

    // Enforce the minimum near plane distance
    float nearDistance = std::max(projection.nearDistance(), span.nearDistance);
    float farDistance = std::min(projection.farDistance(), span.farDistance);
    if (farDistance <= nearDistance)
    {
        // Entire span lies in front of or behind the view frustum, so skip it
        return;
    }

    bool shadowsOn = false;
    unsigned int omniShadowCount = 0;
    if (m_shadowsEnabled && !m_visibleLightSources.empty())
    {
        // Render shadows from the Sun (currently always the first light source)
        shadowsOn = renderDepthBufferSpanShadows(0, span, m_visibleLightSources[0].cameraRelativePosition);

        // See if there are additional light sources casting shadows.
        for (unsigned int i = 1; i < m_visibleLightSources.size() && omniShadowCount < m_omniShadowMaps.size(); ++i)
        {
            if (m_visibleLightSources[i].lightSource->isShadowCaster())
            {
                renderDepthBufferSpanOmniShadows(omniShadowCount,
                                                 span,
                                                 m_visibleLightSources[i].lightSource,
                                                 m_visibleLightSources[i].cameraRelativePosition);
                ++omniShadowCount;
            }
        }
    }

    // Adjust the far distance slightly to prevent small objects at the back of the view
    // from being clipped due to roundoff errors. The adjustment factor must be larger than
    // one ulp of a 32-bit float, but as small as possible to reduce rendering artifacts
    // caused by frusta that overlap in depth.
    float safeFarDistance = farDistance * (1.0f + 1.0e-6f);

    m_renderContext->setProjection(projection.slice(nearDistance, safeFarDistance));
    Frustum viewFrustum = m_renderContext->frustum();

    // Rendering of some translucent objects is order dependent. We can eliminate the
    // worst artifacts by drawing opaque items first and translucent items second.
    for (int pass = 0; pass < 2; ++pass)
    {
        m_renderContext->setPass(pass == 0 ? RenderContext::OpaquePass : RenderContext::TranslucentPass);

        // Draw all items in the span
        for (unsigned int i = 0; i < span.itemCount; i++)
        {
            const VisibleItem& item = m_visibleItems[span.backItemIndex - i];

            if (pass == 0 || !item.geometry->isOpaque())
            {
                if (shadowsOn && item.geometry->isShadowReceiver())
                {
                    m_renderContext->setShadowMapCount(1);
                }
                else
                {
                    m_renderContext->setShadowMapCount(0);
                }

                if (item.geometry->isShadowReceiver())
                {
                    m_renderContext->setOmniShadowMapCount(omniShadowCount);
                }
                else
                {
                    m_renderContext->setOmniShadowMapCount(0);
                }

                if (m_lighting && !m_lighting->reflectionRegions().empty())
                {
                    m_renderContext->setEnvironmentMap(m_lighting->reflectionRegions().front().cubeMap);
                }
                else
                {
                    m_renderContext->setEnvironmentMap(NULL);
                }
                drawItem(item);
            }
        }

        // Disable all shadows
        m_renderContext->setShadowMapCount(0);
        m_renderContext->setOmniShadowMapCount(0);

        // Draw all splittable items that fall at least partly within this span.
        for (unsigned int i = 0; i < m_splittableItems.size(); ++i)
        {
            const VisibleItem& item = m_splittableItems[m_splittableItems.size() - i - 1];

            if (item.nearDistance < span.farDistance && item.farDistance > span.nearDistance)
            {
                if (pass == 0 || !item.geometry->isOpaque())
                {
                    drawItem(item);
                }
            }
        }
    }
}


// Render all shadow casters in a depth buffer span into the shadow map. Return true if
// any shadows were actually drawn.
//
// Parameters:
//   span - the depth buffer span for which to draw shadows
//   lightPosition - the position of the light source relative to the camera
bool
UniverseRenderer::renderDepthBufferSpanShadows(unsigned int shadowIndex,
                                               const DepthBufferSpan& span,
                                               const Vector3d& lightPosition)
{
    if (!m_shadowsEnabled)
    {
        return false;
    }

    assert(shadowIndex < m_shadowMaps.size());

    // Check for shadow support
    if (!Framebuffer::supported() ||
        m_shadowMaps[shadowIndex].isNull() ||
        !m_shadowMaps[shadowIndex]->isValid())
    {
        return false;
    }

    BoundingSphere<float> shadowReceiverBounds;
    bool shadowCastersPresent = false;

    // Find the minimum radius bounding sphere that contains all of the
    // shadow receivers in this span. Also, determine whether there are
    // any shadow casters in the span.
    for (unsigned int i = 0; i < span.itemCount; ++i)
    {
        const VisibleItem& item = m_visibleItems[span.backItemIndex - i];
        const Geometry* geometry = item.geometry;

        if (geometry->isShadowReceiver())
        {
            shadowReceiverBounds.merge(BoundingSphere<float>(item.cameraRelativePosition.cast<float>(), item.boundingRadius));
        }

        if (geometry->isShadowCaster())
        {
            shadowCastersPresent = true;
        }
    }

    // Don't draw shadows if there are no receivers or no casters
    if (!shadowCastersPresent || shadowReceiverBounds.isEmpty())
    {
        return false;
    }

    glDepthRange(0.0f, 1.0f);
    beginShadowRendering();

    Vector3f shadowGroupCenter = shadowReceiverBounds.center();
    float shadowGroupBoundingRadius = shadowReceiverBounds.radius();

    // Compute the light direction. Here it assumed that all objects in the shadow group
    // are far enough from the light source that the rays are nearly parallel and the
    // light source direction is effectively constant.
    Vector3f lightDirection = (lightPosition + shadowGroupCenter.cast<double>()).cast<float>().normalized();

    // Compute the shadow transform, which will convert coordinates from "shadow group space" to
    // shadow space. Shadow group space has axes aligned with world space but has an origin located
    // at the center of the collection of mutually shadowing objects.
    Matrix4f invCameraTransform = m_renderContext->modelview().matrix().transpose();
    Matrix4f shadowTransform = setupShadowRendering(m_shadowMaps[shadowIndex].ptr(), lightDirection, shadowGroupBoundingRadius);
    shadowTransform = shadowTransform * Transform3f(Translation3f(-shadowGroupCenter)).matrix() * invCameraTransform;

    // Render shadows for all casters
    for (unsigned int i = 0; i < span.itemCount; ++i)
    {
        const VisibleItem& item = m_visibleItems[span.backItemIndex - i];
        const Geometry* geometry = item.geometry;

        if (geometry->isShadowCaster())
        {
            Vector3f itemPosition = item.cameraRelativePosition.cast<float>();
            m_renderContext->pushModelView();
            m_renderContext->translateModelView(itemPosition - shadowGroupCenter);
            m_renderContext->rotateModelView(item.orientation);
            item.geometry->renderShadow(*m_renderContext, m_currentTime);
            m_renderContext->popModelView();
        }
    }

    // Pop the matrices pushed in setupShadowRendering()
    m_renderContext->popProjection();
    m_renderContext->popModelView();

    finishShadowRendering(m_renderSurface.ptr(), m_renderColorMask);

    // Reset the viewport
    glDepthRange(m_depthRangeFront, m_depthRangeBack);
    glViewport(m_renderViewport.x(), m_renderViewport.y(), m_renderViewport.width(), m_renderViewport.height());

    // Set shadow state in the render context
    m_renderContext->setShadowMapMatrix(shadowIndex, shadowTransform);
    m_renderContext->setShadowMap(shadowIndex, m_shadowMaps[shadowIndex]->glFramebuffer());

    return true;
}


// Render all shadow casters in a depth buffer span into the cubic shadow map.
// Return true if any shadows were actually drawn.
//
// Parameters:
//   span - the depth buffer span for which to draw shadows
//   lightPosition - the position of the light source relative to the camera
bool
UniverseRenderer::renderDepthBufferSpanOmniShadows(unsigned int shadowIndex,
                                                   const DepthBufferSpan& span,
                                                   const LightSource* light,
                                                   const Vector3d& lightPosition)
{
    // Check for shadow support
    if (!Framebuffer::supported() || !m_shadowsEnabled)
    {
        return false;
    }

    assert(shadowIndex < m_omniShadowMaps.size());

    BoundingSphere<float> shadowReceiverBounds;
    bool shadowCastersPresent = false;

    // Find the minimum radius bounding sphere that contains all of the
    // shadow receivers in this span. Also, determine whether there are
    // any shadow casters in the span.
    for (unsigned int i = 0; i < span.itemCount; ++i)
    {
        const VisibleItem& item = m_visibleItems[span.backItemIndex - i];
        const Geometry* geometry = item.geometry;

        if (geometry->isShadowReceiver())
        {
            shadowReceiverBounds.merge(BoundingSphere<float>(item.cameraRelativePosition.cast<float>(), item.boundingRadius));
        }

        if (geometry->isShadowCaster())
        {
            shadowCastersPresent = true;
        }
    }

    // Don't draw shadows if there are no receivers or no casters
    if (!shadowCastersPresent || shadowReceiverBounds.isEmpty())
    {
        return false;
    }

    // Set up the view port (same for all faces)
    glViewport(0, 0, m_omniShadowMaps[shadowIndex]->size(), m_omniShadowMaps[shadowIndex]->size());
    glDepthRange(0.0f, 1.0f);

    // Set up cube map shadow rendering
    // When rendering to cube faces, we use a left-handed projection, so reverse the triangles (GL_CW)
    // Also, tell the renderer to output camera distance instead of color
    beginCubicShadowRendering();
    glFrontFace(GL_CW);
    m_renderContext->setRendererOutput(RenderContext::CameraDistance);

    // Pixel distance is stored in the red channel; clear it to a very large value
    glClearColor(1.0e15f, 0.0f, 0.0f, 0.0f);

    m_renderContext->pushProjection();

    // Draw each face of the cube map. Frustum cull objects to avoid unnecessary
    // redrawing.
    for (int face = 0; face < 6; ++face)
    {
        Framebuffer* fb = m_omniShadowMaps[shadowIndex]->face(CubeMapFramebuffer::Face(face));
        if (fb)
        {
            fb->bind();
            glDepthMask(GL_TRUE);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            Quaternionf cameraOrientation = CubeFaceCameraRotations[face].cast<float>();
            Matrix3f toCameraSpace = cameraOrientation.conjugate().toRotationMatrix();

            // Set the camera transformation
            m_renderContext->pushModelView();
            m_renderContext->setModelView(Matrix4f::Identity());
            m_renderContext->rotateModelView(cameraOrientation.conjugate().cast<float>());

            // The camera orientation is stored separately; save it so that we can restore
            // it after rendering all faces.
            Quaternionf savedCamera = m_renderContext->cameraOrientation();
            m_renderContext->setCameraOrientation(cameraOrientation);

            PlanarProjection faceProjection = PlanarProjection::CreatePerspectiveLH(float(toRadians(90.0)), 1.0f, light->range() * 0.0001f, light->range());
            Frustum faceFrustum = faceProjection.frustum();

            m_renderContext->setProjection(faceProjection);

            // Render shadows for all casters
            for (unsigned int i = 0; i < span.itemCount; ++i)
            {
                const VisibleItem& item = m_visibleItems[span.backItemIndex - i];
                const Geometry* geometry = item.geometry;

                if (geometry->isShadowCaster())
                {
                    Vector3f itemPosition = (item.cameraRelativePosition - lightPosition).cast<float>();
                    Vector3f cameraSpacePosition = toCameraSpace * itemPosition;

                    // Test object bounding sphere against cube face frustum
                    if (faceFrustum.intersects(BoundingSphere<float>(cameraSpacePosition, light->range())))
                    {
                        m_renderContext->pushModelView();
                        m_renderContext->translateModelView(itemPosition);
                        m_renderContext->rotateModelView(item.orientation);
                        item.geometry->renderShadow(*m_renderContext, m_currentTime);
                        m_renderContext->popModelView();
                    }
                }
            }

            m_renderContext->popModelView();
            m_renderContext->setCameraOrientation(savedCamera);
        }
    }

    m_renderContext->popProjection();

    // Restore normal renderer operation
    m_renderContext->setRendererOutput(RenderContext::FragmentColor);
    finishShadowRendering(m_renderSurface.ptr(), m_renderColorMask);
    glFrontFace(GL_CCW);

    // Reset the viewport
    glDepthRange(m_depthRangeFront, m_depthRangeBack);
    glViewport(m_renderViewport.x(), m_renderViewport.y(), m_renderViewport.width(), m_renderViewport.height());

    // Set shadow state in the render context
    m_renderContext->setOmniShadowMap(shadowIndex, m_omniShadowMaps[shadowIndex]->colorTexture());

    // Restore clear color to black
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    return true;
}


void
UniverseRenderer::drawItem(const VisibleItem& item)
{       
    m_renderContext->setModelTranslation(m_renderContext->modelview().linear().cast<double>() * item.cameraRelativePosition);

    // Set up the light sources
    unsigned int lightCount = 0;
    if (!m_lightSources.empty())
    {
        for (vector<VisibleLightSourceItem>::const_iterator iter = m_visibleLightSources.begin(); iter != m_visibleLightSources.end(); ++iter)
        {
            if (!iter->lightSource)
            {
                // Special case for the Sun
                m_renderContext->setLight(0, RenderContext::Light(RenderContext::DirectionalLight,
                                                                  iter->cameraRelativePosition.cast<float>(),
                                                                  Spectrum(1.0f, 1.0f, 1.0f),
                                                                  1.0f));
                ++lightCount;
            }
            else
            {
                Vector3f lightPosition = (iter->position - item.position).cast<float>();
                float distanceToLight = lightPosition.norm() - item.boundingRadius;
                float attenuation = 1.0f / (256.0f * iter->lightSource->range() * iter->lightSource->range());
                if (distanceToLight < iter->lightSource->range())
                {
                    m_renderContext->setLight(lightCount,
                                              RenderContext::Light(RenderContext::PointLight,
                                                                   iter->cameraRelativePosition.cast<float>(),
                                                                   iter->lightSource->spectrum(),
                                                                   attenuation));
                    ++lightCount;
                }
            }
        }
    }

    m_renderContext->setActiveLightCount(lightCount);

    m_renderContext->pushModelView();
    m_renderContext->translateModelView(item.cameraRelativePosition.cast<float>());
    m_renderContext->rotateModelView(item.orientation);

    if (!item.outsideFrustum)
    {
        item.geometry->render(*m_renderContext, m_currentTime);
    }

    m_renderContext->popModelView();
}


/** Set the color of 'fill light' in the scene. Ambient light is
  * a crude approximation to the light resulting from multiple
  * reflections off of diffuse surfaces. By default, the ambient
  * light is set to black. The default is realistic for space scenes, but
  * some ambient light may be desirable when clarity of the visualization
  * is more important than realism.
  */
void
UniverseRenderer::setAmbientLight(const Spectrum& spectrum)
{
    m_ambientLight = spectrum;
}


// Create a view matrix for drawing the scene from the
// point of view of a light source.
static Matrix4f
shadowView(const Vector3f& lightDirection)
{
    Vector3f u = lightDirection.unitOrthogonal();
    Vector3f v = u.cross(lightDirection);
    Matrix4f lightView;
    lightView << v.transpose(),              0.0,
                 u.transpose(),              0.0,
                 lightDirection.transpose(), 0.0,
                 0.0, 0.0, 0.0,  1.0;

    return lightView;
}


// Shadow bias matrix for mapping to a unit cube with
// one corner at the origin (since texture coordinates
// are in [ 0, 1 ] instead of [ -1, 1 ])
static Matrix4f
shadowBias()
{
    Matrix4f bias;
    bias << 0.5f, 0.0f, 0.0f, 0.5f,
            0.0f, 0.5f, 0.0f, 0.5f,
            0.0f, 0.0f, 0.5f, 0.5f,
            0.0f, 0.0f, 0.0f, 1.0f;

    return bias;
}


// Set up graphics state for rendering shadows. Return the matrix that
// should be used for drawing geometry with this shadow map.
Matrix4f
UniverseRenderer::setupShadowRendering(const Framebuffer* shadowMap,
                                       const Vector3f& lightDirection,
                                       float shadowGroupSize)
{
    if (!shadowMap->isValid())
    {
        return Matrix4f::Identity();
    }

    shadowMap->bind();

#if DEBUG_SHADOW_MAP
    GLenum errCode = glGetError();
    if (errCode != GL_NO_ERROR)
    {
        VESTA_LOG("glError in shadow setup: %s", gluErrorString(errCode));
    }
#endif

    PlanarProjection shadowProjection = PlanarProjection::CreateOrthographic(-shadowGroupSize, shadowGroupSize,
                                                                             -shadowGroupSize, shadowGroupSize,
                                                                             -shadowGroupSize, shadowGroupSize);
    Matrix4f modelView = shadowView(lightDirection);

    glClear(GL_DEPTH_BUFFER_BIT);

    m_renderContext->pushProjection();
    m_renderContext->setProjection(shadowProjection);
    m_renderContext->pushModelView();
    m_renderContext->setModelView(modelView);

    glViewport(0, 0, shadowMap->width(), shadowMap->height());
    glDepthRange(0.0f, 1.0f);

    return shadowBias() * shadowProjection.matrix() * modelView;
}



