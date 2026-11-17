#if MLN_RENDER_BACKEND_OPENGL

#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/gl/defines.hpp>
#include <mln/map/map_options.hpp>
#include <mln/platform/gl_functions.hpp>
#include <mln/style/layers/custom_layer.hpp>
#include <mln/style/style.hpp>
#include <mln/style/terrain.hpp>
#include <mln/util/image.hpp>
#include <mln/util/mat4.hpp>
#include <mln/util/run_loop.hpp>

#include <cmath>
#include <memory>
#include <optional>
#include <string>

using namespace mln;
using namespace mln::style;
using namespace mln::platform;

namespace {

// A custom layer drawing onto the terrain in one screen-sized pass: each pixel's place on the ground
// unprojected from CustomLayerRenderParameters::terrainDepthTexture through the inverse of the
// projection matrix, written as its offset from the map's centre in Mercator (scaled by 1 / span
// around 0.5 in red and green).
const GLchar* vertexSource = R"(#version 300 es
in vec2 a_pos;
void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }
)";

const GLchar* fragmentSource = R"(#version 300 es
precision highp float;
uniform highp sampler2D u_depth;
uniform highp mat4 u_inv;   // clip -> Mercator less the centre
uniform highp float u_span;
out vec4 fragColor;
void main() {
    vec4 k = texelFetch(u_depth, ivec2(gl_FragCoord.xy), 0) * 255.0;
    float depth = dot(k, vec4(1.0 / 256.0, 1.0 / 65536.0, 1.0 / 16777216.0, 1.0 / 4294967296.0));
    if (depth > 0.9999999) { fragColor = vec4(0.0, 0.0, 1.0, 1.0); return; }
    vec2 ndc = gl_FragCoord.xy / vec2(textureSize(u_depth, 0)) * 2.0 - 1.0;
    vec4 g = u_inv * vec4(ndc, depth * 2.0 - 1.0, 1.0);
    vec2 offset = g.xy / g.w / u_span + 0.5;
    fragColor = vec4(clamp(offset, 0.0, 1.0), 0.0, 1.0);
}
)";

constexpr double span = 0.004; // Mercator, the colours' range

class GroundLayer : public CustomLayerHost {
public:
    void initialize(const CustomLayerInitParameters&) override {
        program = MBGL_CHECK_ERROR(glCreateProgram());
        const auto compile = [&](GLenum type, const GLchar* source) {
            const GLuint shader = MBGL_CHECK_ERROR(glCreateShader(type));
            MBGL_CHECK_ERROR(glShaderSource(shader, 1, &source, nullptr));
            MBGL_CHECK_ERROR(glCompileShader(shader));
            MBGL_CHECK_ERROR(glAttachShader(program, shader));
            return shader;
        };
        vertexShader = compile(GL_VERTEX_SHADER, vertexSource);
        fragmentShader = compile(GL_FRAGMENT_SHADER, fragmentSource);
        MBGL_CHECK_ERROR(glLinkProgram(program));
        GLint linked = 0;
        MBGL_CHECK_ERROR(glGetProgramiv(program, GL_LINK_STATUS, &linked));
        programLinked = linked != 0;
        a_pos = MBGL_CHECK_ERROR(glGetAttribLocation(program, "a_pos"));
        const GLfloat triangle[] = {-1, -1, 3, -1, -1, 3};
        MBGL_CHECK_ERROR(glGenBuffers(1, &buffer));
        MBGL_CHECK_ERROR(glBindBuffer(GL_ARRAY_BUFFER, buffer));
        MBGL_CHECK_ERROR(glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW));
    }

    void render(const CustomLayerRenderParameters& parameters) override {
        rendered = true;
        depthTexture = parameters.terrainDepthTexture;
        exaggeration = parameters.terrainExaggeration;
        if (!depthTexture) {
            return;
        }
        // Mercator (0..1) to clip, in double, and its inverse less the centre
        const double worldSize = 512.0 * std::pow(2.0, parameters.zoom);
        mat4 mercator;
        for (size_t i = 0; i < 16; ++i) {
            mercator[i] = parameters.projectionMatrix[i] * (i < 8 ? worldSize : 1.0);
        }
        mat4 inverse;
        if (matrix::invert(inverse, mercator)) { // true: singular
            return;
        }
        const double lat = parameters.latitude * M_PI / 180.0;
        centreX = (parameters.longitude + 180.0) / 360.0;
        centreY = 0.5 - std::log(std::tan(M_PI / 4 + lat / 2)) / (2 * M_PI);
        for (size_t c = 0; c < 4; ++c) {
            inverse[c * 4] -= centreX * inverse[c * 4 + 3];
            inverse[c * 4 + 1] -= centreY * inverse[c * 4 + 3];
        }
        clipToGround = inverse;
        metersPerPixel = 40075016.68557849 * std::cos(lat) / worldSize;
        width = parameters.width;
        height = parameters.height;

        GLfloat inv[16];
        for (size_t i = 0; i < 16; ++i) {
            inv[i] = static_cast<GLfloat>(inverse[i]);
        }
        MBGL_CHECK_ERROR(glUseProgram(program));
        MBGL_CHECK_ERROR(glActiveTexture(GL_TEXTURE0));
        MBGL_CHECK_ERROR(glBindTexture(GL_TEXTURE_2D, depthTexture));
        MBGL_CHECK_ERROR(glUniform1i(glGetUniformLocation(program, "u_depth"), 0));
        MBGL_CHECK_ERROR(glUniformMatrix4fv(glGetUniformLocation(program, "u_inv"), 1, GL_FALSE, inv));
        MBGL_CHECK_ERROR(glUniform1f(glGetUniformLocation(program, "u_span"), static_cast<GLfloat>(span)));
        MBGL_CHECK_ERROR(glDisable(GL_DEPTH_TEST));
        MBGL_CHECK_ERROR(glDisable(GL_BLEND));
        MBGL_CHECK_ERROR(glBindBuffer(GL_ARRAY_BUFFER, buffer));
        MBGL_CHECK_ERROR(glEnableVertexAttribArray(a_pos));
        MBGL_CHECK_ERROR(glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
        MBGL_CHECK_ERROR(glDrawArrays(GL_TRIANGLES, 0, 3));
        MBGL_CHECK_ERROR(glDisableVertexAttribArray(a_pos));
    }

    void contextLost() override {}

    void deinitialize() override {
        if (program) {
            MBGL_CHECK_ERROR(glDeleteBuffers(1, &buffer));
            MBGL_CHECK_ERROR(glDeleteShader(vertexShader));
            MBGL_CHECK_ERROR(glDeleteShader(fragmentShader));
            MBGL_CHECK_ERROR(glDeleteProgram(program));
        }
    }

    // Where the ray through a pixel (top left origin) meets the ground at `meters` above the sea,
    // less the centre, from the same inverse in double: what the layer's colour there should say
    std::optional<std::pair<double, double>> groundAt(double px, double py, double meters) const {
        const double ndcX = (px + 0.5) / width * 2.0 - 1.0;
        const double ndcY = 1.0 - (py + 0.5) / height * 2.0;
        const auto unproject = [&](double z) {
            vec4 out;
            matrix::transformMat4(out, vec4{{ndcX, ndcY, z, 1.0}}, clipToGround);
            return std::array<double, 3>{out[0] / out[3], out[1] / out[3], out[2] / out[3]};
        };
        const auto nearPoint = unproject(-1.0);
        const auto farPoint = unproject(1.0);
        if (nearPoint[2] == farPoint[2]) {
            return std::nullopt;
        }
        const double t = (meters - nearPoint[2]) / (farPoint[2] - nearPoint[2]);
        if (t < 0.0 || t > 1.0) {
            return std::nullopt;
        }
        return std::make_pair(nearPoint[0] + t * (farPoint[0] - nearPoint[0]),
                              nearPoint[1] + t * (farPoint[1] - nearPoint[1]));
    }

    GLuint program = 0, vertexShader = 0, fragmentShader = 0, buffer = 0;
    GLint a_pos = 0;
    bool programLinked = false;
    bool rendered = false;
    unsigned int depthTexture = 0;
    double exaggeration = 0;
    mat4 clipToGround{};
    double centreX = 0, centreY = 0, metersPerPixel = 1, width = 1, height = 1;
};

// A DEM tile of `meters` everywhere, Mapbox's encoding: (h + 10000) / 0.1
std::string flatDEMTile(double meters) {
    const auto v = static_cast<uint32_t>(std::lround((meters + 10000.0) / 0.1));
    PremultipliedImage image({256, 256});
    for (uint32_t i = 0; i < 256 * 256; ++i) {
        image.data[i * 4 + 0] = static_cast<uint8_t>(v >> 16);
        image.data[i * 4 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        image.data[i * 4 + 2] = static_cast<uint8_t>(v & 0xFF);
        image.data[i * 4 + 3] = 255;
    }
    return encodePNG(image);
}

const char* terrainStyle = R"({
  "version": 8,
  "sources": {
    "dem": {"type": "raster-dem", "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"], "maxzoom": 9, "tileSize": 256}
  },
  "layers": [{"id": "background", "type": "background", "paint": {"background-color": "black"}}]
})";

// How many of the sampled pixels say the place on the ground the ray through them meets, within
// `tolerance` of the colours' range, and how many were sampled
std::pair<int, int> pixelsOnTheGround(const PremultipliedImage& image, const GroundLayer& layer, double meters) {
    int good = 0, sampled = 0;
    const double tolerance = 3.0 / 255.0;
    for (uint32_t y = 8; y < image.size.height - 8; y += 16) {
        for (uint32_t x = 8; x < image.size.width - 8; x += 16) {
            const auto ground = layer.groundAt(x, y, meters);
            if (!ground) {
                continue;
            }
            const double r = ground->first / span + 0.5;
            const double g = ground->second / span + 0.5;
            if (r < 0.02 || r > 0.98 || g < 0.02 || g > 0.98) {
                continue; // beyond the colours' range
            }
            const uint8_t* px = image.data.get() + (y * image.size.width + x) * 4;
            ++sampled;
            if (std::abs(px[0] / 255.0 - r) <= tolerance && std::abs(px[1] / 255.0 - g) <= tolerance) {
                ++good;
            }
        }
    }
    return {good, sampled};
}

} // namespace

// The terrain depth pass is handed to an OpenGL custom layer, which unprojects it into the place on
// the ground of every pixel: on ground 2000 m high, the place the ray through the pixel meets that
// height, not the sea's.
TEST(CustomLayer, TerrainDepthGivesThePlaceOnTheGround) {
    if (gfx::Backend::GetType() != gfx::Backend::Type::OpenGL) {
        return;
    }
    constexpr double meters = 2000.0;
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    const std::string dem = flatDEMTile(meters);
    fileSource->tileResponse = [&](const Resource&) -> std::optional<Response> {
        Response res;
        res.data = std::make_shared<std::string>(dem);
        return res;
    };
    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(terrainStyle);
    map.getStyle().setTerrain(std::make_unique<style::Terrain>("dem", 1.0f));
    auto host = std::make_unique<GroundLayer>();
    GroundLayer& layer = *host;
    map.getStyle().addLayer(std::make_unique<CustomLayer>("ground", std::move(host)));
    map.jumpTo(CameraOptions().withCenter(LatLng{45.9, 6.9}).withZoom(10.0).withPitch(55.0).withBearing(30.0));

    const PremultipliedImage image = frontend.render(map).image;

    ASSERT_TRUE(layer.programLinked);
    ASSERT_TRUE(layer.rendered);
    ASSERT_NE(layer.depthTexture, 0u) << "the custom layer gets the terrain depth texture";
    EXPECT_DOUBLE_EQ(layer.exaggeration, 1.0);

    const auto [good, sampled] = pixelsOnTheGround(image, layer, meters);
    ASSERT_GT(sampled, 50);
    EXPECT_GT(good, sampled * 95 / 100) << good << " of " << sampled << " pixels on the 2000 m ground";
    // and not on the sea's: the same pixels unprojected onto sea level mostly disagree
    const auto [atSea, sampledAtSea] = pixelsOnTheGround(image, layer, 0.0);
    EXPECT_LT(atSea, sampledAtSea / 2) << atSea << " of " << sampledAtSea << " pixels at sea level";
}

// Without terrain, nothing is handed over.
TEST(CustomLayer, NoTerrainDepthWithoutTerrain) {
    if (gfx::Backend::GetType() != gfx::Backend::Type::OpenGL) {
        return;
    }
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    HeadlessFrontend frontend{{64, 64}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(terrainStyle);
    auto host = std::make_unique<GroundLayer>();
    GroundLayer& layer = *host;
    map.getStyle().addLayer(std::make_unique<CustomLayer>("ground", std::move(host)));
    frontend.render(map);
    ASSERT_TRUE(layer.rendered);
    EXPECT_EQ(layer.depthTexture, 0u);
    EXPECT_EQ(layer.exaggeration, 0.0);
}

#endif
