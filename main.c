#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <string.h>

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "external/tinyexpr/tinyexpr.h"

#define CLAY_IMPLEMENTATION
#include "external/clay/clay.h"
#include "external/clay/clay_renderer_SDL3.c"
#include "external/clay/clay-video-demo.c"

static const Uint32 FONT_ID = 0;

static const Clay_Color COLOR_ORANGE    = (Clay_Color) {225, 138, 50, 255};
static const Clay_Color COLOR_BLUE      = (Clay_Color) {111, 173, 162, 255};
static const Clay_Color COLOR_LIGHT     = (Clay_Color) {224, 215, 210, 255};

/* =========================
   Graph Renderer Components
   ========================= */

typedef struct {
    double cx;      // center X in math space
    double cy;      // center Y in math space
    double xScale;  // pixels per unit
    double yScale;
} Viewport;

typedef struct {
    double x, y;
} Vec2d;

typedef struct {
    SDL_Texture *graph_texture;
    Viewport viewport;
    Vec2d velocity;
    char function[256];
    bool needs_update;
} GraphState;

typedef struct app_state {
    SDL_Window *window;
    Clay_SDL3RendererData rendererData;
    ClayVideoDemo_Data demoData;
    GraphState graphState;
} AppState;

SDL_Texture *sample_image;
bool show_demo = false;
bool show_graph = true;

/* =========================
   Graph Rendering Functions
   ========================= */

static inline SDL_FPoint math_to_screen(const Viewport *v, double x, double y, int width, int height)
{
    SDL_FPoint p;
    p.x = (float)(width  / 2.0 + (x - v->cx) * v->xScale);
    p.y = (float)(height / 2.0 - (y - v->cy) * v->yScale);
    return p;
}

char *expand_implicit_mul(const char *src)
{
    size_t len = strlen(src);
    char *out = malloc(len * 2 + 1);
    char *p = out;

    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        *p++ = c;

        if (i + 1 < len) {
            char n = src[i + 1];

            bool left  = isdigit(c) || c == ')' || c == 'x';
            bool right = n == '(' || n == 'x' || isalpha(n);

            if (left && right)
                *p++ = '*';
        }
    }
    *p = '\0';
    return out;
}

int drawGraph(SDL_Renderer *r, const Viewport *v, const char *func, int width, int height)
{
    const int samples = width;
    SDL_FPoint *points = malloc(sizeof(SDL_FPoint) * samples);
    if (!points) return -1;

    double halfW = (width / 2.0) / v->xScale;
    double xMin = v->cx - halfW;
    double xMax = v->cx + halfW;

    double x;
    te_variable vars[] = {{"x", &x}};
    int err;

    char *expanded = expand_implicit_mul(func);
    te_expr *expr = te_compile(expanded, vars, 1, &err);
    free(expanded);

    if (!expr || err) {
        SDL_Log("Expression error");
        free(points);
        return -1;
    }

    for (int i = 0; i < samples; i++) {
        double t = (double)i / (samples - 1);
        x = xMin + t * (xMax - xMin);
        double y = te_eval(expr);
        points[i] = math_to_screen(v, x, y, width, height);
    }

    te_free(expr);
    SDL_RenderLines(r, points, samples);
    free(points);
    return 0;
}

double grid_step(double scale)
{
    double target = 80.0;
    double step = pow(10, floor(log10(target / scale)));
    if (step * scale < target / 2) step *= 2;
    if (step * scale > target * 2) step /= 2;
    return step;
}

void draw_grid(SDL_Renderer *r, const Viewport *v, int width, int height)
{
    double step = grid_step(v->xScale);

    double hw = (width  / 2.0) / v->xScale;
    double hh = (height / 2.0) / v->yScale;

    double xStart = floor((v->cx - hw) / step) * step;
    double xEnd   = ceil ((v->cx + hw) / step) * step;
    double yStart = floor((v->cy - hh) / step) * step;
    double yEnd   = ceil ((v->cy + hh) / step) * step;

    SDL_SetRenderDrawColor(r, 40, 40, 40, 255);

    for (double x = xStart; x <= xEnd; x += step) {
        SDL_FPoint a = math_to_screen(v, x, yStart, width, height);
        SDL_FPoint b = math_to_screen(v, x, yEnd, width, height);
        SDL_RenderLines(r, (SDL_FPoint[]){a, b}, 2);
    }

    for (double y = yStart; y <= yEnd; y += step) {
        SDL_FPoint a = math_to_screen(v, xStart, y, width, height);
        SDL_FPoint b = math_to_screen(v, xEnd,   y, width, height);
        SDL_RenderLines(r, (SDL_FPoint[]){a, b}, 2);
    }
}

void draw_axes(SDL_Renderer *r, const Viewport *v, int width, int height)
{
    SDL_SetRenderDrawColor(r, 160, 160, 160, 255);

    double hw = (width  / 2.0) / v->xScale;
    double hh = (height / 2.0) / v->yScale;

    SDL_FPoint xAxis[2] = {
        math_to_screen(v, v->cx - hw, 0, width, height),
        math_to_screen(v, v->cx + hw, 0, width, height)
    };

    SDL_FPoint yAxis[2] = {
        math_to_screen(v, 0, v->cy - hh, width, height),
        math_to_screen(v, 0, v->cy + hh, width, height)
    };

    SDL_RenderLines(r, xAxis, 2);
    SDL_RenderLines(r, yAxis, 2);
}

SDL_Texture* render_graph_to_texture(
    SDL_Renderer *renderer,
    const char *function,
    const Viewport *viewport,
    int width, 
    int height,
    TTF_Font *label_font,
    Uint32 label_font_id)
{
    if (!function || !viewport || width <= 0 || height <= 0) {
        return NULL;
    }

    // Create an RGBA surface we will draw into
    SDL_Surface *surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        SDL_Log("Failed to create surface: %s", SDL_GetError());
        return NULL;
    }

    // Make a software renderer for the surface so we can reuse existing draw helpers
    SDL_Renderer *soft_renderer = SDL_CreateSoftwareRenderer(surface);
    if (!soft_renderer) {
        SDL_Log("Failed to create software renderer: %s", SDL_GetError());
        SDL_DestroySurface(surface);
        return NULL;
    }

    // Clear background
    SDL_SetRenderDrawColor(soft_renderer, 0, 0, 0, 255);
    SDL_RenderClear(soft_renderer);

    // Draw grid, axes, and function
    draw_grid(soft_renderer, viewport, width, height);
    draw_axes(soft_renderer, viewport, width, height);

    SDL_SetRenderDrawColor(soft_renderer, 0, 255, 0, 255);
    drawGraph(soft_renderer, viewport, function, width, height);

    // Present the renderer before we start blitting text
    SDL_RenderPresent(soft_renderer);
    SDL_DestroyRenderer(soft_renderer);

    // Now add labels by blitting text surfaces directly
    SDL_Color label_color = { 200, 200, 200, 255 };

    // --- Compute grid step & visible ranges ---
    double step = grid_step(viewport->xScale);
    double halfW = (width / 2.0) / viewport->xScale;
    double halfH = (height / 2.0) / viewport->yScale;
    double xStart = floor((viewport->cx - halfW) / step) * step;
    double xEnd   = ceil ((viewport->cx + halfW) / step) * step;
    double yStart = floor((viewport->cy - halfH) / step) * step;
    double yEnd   = ceil ((viewport->cy + halfH) / step) * step;

    // Axis screen positions
    SDL_FPoint axis_origin = math_to_screen(viewport, 0.0, 0.0, width, height);
    int axis_y = (int)axis_origin.y;
    int axis_x = (int)axis_origin.x;

    // Label padding
    const int pad = 4;

    // --- X axis labels ---
    for (double x = xStart; x <= xEnd; x += step) {
        if (fabs(x) < 1e-9) continue; // Skip origin

        char buf[64];
        snprintf(buf, sizeof(buf), "%.6g", x);

        SDL_FPoint p = math_to_screen(viewport, x, 0.0, width, height);
        int px = (int)roundf(p.x);

        int ty;
        if (axis_y >= 0 && axis_y <= height) {
            ty = axis_y + pad;
            if (ty + 1 > height - pad) ty = height - pad - 12;
        } else {
            ty = (axis_y < 0) ? pad : (height - pad - 12);
        }

        if (label_font) {
            SDL_Surface *ts = TTF_RenderText_Blended(label_font, buf, strlen(buf), label_color);
            if (ts) {
                int tx = px - ts->w / 2;
                SDL_Rect dst = { tx, ty, 0, 0 };
                
                if (dst.x < 0) dst.x = 0;
                if (dst.x + ts->w > width) dst.x = width - ts->w;
                if (dst.y < 0) dst.y = 0;
                if (dst.y + ts->h > height) dst.y = height - ts->h;
                
                SDL_BlitSurface(ts, NULL, surface, &dst);
                SDL_DestroySurface(ts);
            }
        }
    }

    // --- Y axis labels ---
    for (double y = yStart; y <= yEnd; y += step) {
        if (fabs(y) < 1e-9) continue; // Skip origin

        char buf[64];
        snprintf(buf, sizeof(buf), "%.6g", y);

        SDL_FPoint p = math_to_screen(viewport, 0.0, y, width, height);
        int py = (int)roundf(p.y);

        int tx;
        if (axis_x >= 0 && axis_x <= width) {
            tx = axis_x + pad;
            if (tx + 1 > width - pad) tx = width - pad - 30;
        } else {
            tx = (axis_x < 0) ? pad : (width - pad - 48);
        }

        if (label_font) {
            SDL_Surface *ts = TTF_RenderText_Blended(label_font, buf, strlen(buf), label_color);
            if (ts) {
                SDL_Rect dst = { tx, py - ts->h / 2, 0, 0 };
                
                if (dst.x < 0) dst.x = 0;
                if (dst.x + ts->w > width) dst.x = width - ts->w;
                if (dst.y < 0) dst.y = 0;
                if (dst.y + ts->h > height) dst.y = height - ts->h;
                
                SDL_BlitSurface(ts, NULL, surface, &dst);
                SDL_DestroySurface(ts);
            }
        }
    }

    // Create a texture from the surface for GPU blitting later
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);

    return texture;
}

void update_graph_texture(AppState *state, int width, int height)
{
    if (state->graphState.graph_texture) {
        SDL_DestroyTexture(state->graphState.graph_texture);
    }

    TTF_Font *label_font = NULL;
    if (state->rendererData.fonts) label_font = state->rendererData.fonts[FONT_ID];

    state->graphState.graph_texture = render_graph_to_texture(
        state->rendererData.renderer,
        state->graphState.function,
        &state->graphState.viewport,
        width, height,
        label_font,
        FONT_ID
    );

    state->graphState.needs_update = false;
}

void update_graph_movement(GraphState *gs, double dt)
{
    int n;
    const bool *keys = SDL_GetKeyboardState(&n);
    const double speed = 5.0;

    gs->velocity.x = gs->velocity.y = 0;
    if (keys[SDL_SCANCODE_A]) gs->velocity.x -= speed;
    if (keys[SDL_SCANCODE_D]) gs->velocity.x += speed;
    if (keys[SDL_SCANCODE_W]) gs->velocity.y += speed;
    if (keys[SDL_SCANCODE_S]) gs->velocity.y -= speed;

    gs->viewport.cx += gs->velocity.x * dt;
    gs->viewport.cy += gs->velocity.y * dt;

    if (gs->velocity.x != 0 || gs->velocity.y != 0) {
        gs->needs_update = true;
    }
}

void update_graph_zoom(GraphState *gs, double dt)
{
    int n;
    const bool *keys = SDL_GetKeyboardState(&n);
    const double zoomSpeed = 1.5;

    bool zoomed = false;

    if (keys[SDL_SCANCODE_Z]) {
        double factor = exp(zoomSpeed * dt);
        gs->viewport.xScale *= factor;
        gs->viewport.yScale *= factor;
        zoomed = true;
    }
    if (keys[SDL_SCANCODE_X]) {
        double factor = exp(-zoomSpeed * dt);
        gs->viewport.xScale *= factor;
        gs->viewport.yScale *= factor;
        zoomed = true;
    }

    if (gs->viewport.xScale < 10)  gs->viewport.xScale = gs->viewport.yScale = 10;
    if (gs->viewport.xScale > 500) gs->viewport.xScale = gs->viewport.yScale = 500;

    if (zoomed) {
        gs->needs_update = true;
    }
}

/* =========================
   Clay UI Functions
   ========================= */

static inline Clay_Dimensions SDL_MeasureText(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData)
{
    TTF_Font **fonts = userData;
    TTF_Font *font = fonts[config->fontId];
    int width, height;

    TTF_SetFontSize(font, config->fontSize);
    if (!TTF_GetStringSize(font, text.chars, text.length, &width, &height)) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to measure text: %s", SDL_GetError());
    }

    return (Clay_Dimensions) { (float) width, (float) height };
}

void HandleClayErrors(Clay_ErrorData errorData) {
    printf("%s", errorData.errorText.chars);
}

Clay_RenderCommandArray ClayImageSample_CreateLayout(AppState *state) {
    Clay_BeginLayout();

    Clay_Sizing layoutExpand = {
        .width = CLAY_SIZING_GROW(0),
        .height = CLAY_SIZING_GROW(0)
    };

    CLAY(CLAY_ID("OuterContainer"), {
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .sizing = layoutExpand,
            .padding = CLAY_PADDING_ALL(16),
            .childGap = 16
        }
    }) {
        CLAY(CLAY_ID("SampleImage"), {
            .layout = {
                .sizing = layoutExpand
            },
            .aspectRatio = { 23.0 / 42.0 },
            .image = {
                .imageData = sample_image,
            }
        });
    }

    return Clay_EndLayout();
}

Clay_RenderCommandArray ClayGraph_CreateLayout(AppState *state) {
    Clay_BeginLayout();

    Clay_Sizing layoutExpand = {
        .width = CLAY_SIZING_GROW(0),
        .height = CLAY_SIZING_GROW(0)
    };

    CLAY(CLAY_ID("OuterContainer"), {
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .sizing = layoutExpand,
            .padding = CLAY_PADDING_ALL(16),
            .childGap = 16
        },
        .backgroundColor = COLOR_LIGHT
    }) {
        // Title
        static char title[512];
        snprintf(title, sizeof(title), "Graph Plotter: %s", state->graphState.function);
        Clay_String titleString = {
            .chars = title, 
            .length = strlen(title),
            .isStaticallyAllocated = true
        };

        CLAY_TEXT(titleString, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID,
            .fontSize = 32,
            .textColor = {50, 50, 50, 255}
        }));

        // Instructions
        CLAY_TEXT(CLAY_STRING("WASD to pan • Z/X to zoom • Space to toggle"), CLAY_TEXT_CONFIG({
            .fontId = FONT_ID,
            .fontSize = 16,
            .textColor = {100, 100, 100, 255}
        }));

        // Graph display
        CLAY(CLAY_ID("GraphContainer"), {
            .layout = {
                .sizing = layoutExpand
            },
            .image = {
                .imageData = state->graphState.graph_texture,
            }
        });

        // Function display
        static char func_display[512];
        snprintf(func_display, sizeof(func_display), "f(x) = %s", state->graphState.function);
        Clay_String funcString = {
            .chars = func_display,
            .length = strlen(func_display),
            .isStaticallyAllocated = true
        };

        CLAY_TEXT(funcString, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID,
            .fontSize = 20,
            .textColor = {50, 50, 50, 255}
        }));
    }

    return Clay_EndLayout();
}

static const char *get_cmd_arg(int argc, char *argv[], const char *prefix)
{
    size_t len = strlen(prefix);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], prefix, len) == 0) {
            return argv[i] + len;
        }
    }
    return NULL;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    if (!TTF_Init()) {
        return SDL_APP_FAILURE;
    }

    AppState *state = SDL_calloc(1, sizeof(AppState));
    if (!state) {
        return SDL_APP_FAILURE;
    }
    *appstate = state;

    if (!SDL_CreateWindowAndRenderer("Clay + Graph Demo", 800, 600, 0, 
                                     &state->window, &state->rendererData.renderer)) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to create window and renderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_SetWindowResizable(state->window, true);

    state->rendererData.textEngine = TTF_CreateRendererTextEngine(state->rendererData.renderer);
    if (!state->rendererData.textEngine) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to create text engine: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    state->rendererData.fonts = SDL_calloc(1, sizeof(TTF_Font *));
    if (!state->rendererData.fonts) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to allocate font array: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    TTF_Font *font = TTF_OpenFont("external/resources/Roboto-Regular.ttf", 24);
    if (!font) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to load font: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    state->rendererData.fonts[FONT_ID] = font;

    sample_image = IMG_LoadTexture(state->rendererData.renderer, "resources/sample.png");
    if (!sample_image) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to load image: %s", SDL_GetError());
    }

    // Initialize Clay
    uint64_t totalMemorySize = Clay_MinMemorySize();
    Clay_Arena clayMemory = (Clay_Arena) {
        .memory = SDL_malloc(totalMemorySize),
        .capacity = totalMemorySize
    };

    int width, height;
    SDL_GetWindowSize(state->window, &width, &height);
    Clay_Initialize(clayMemory, (Clay_Dimensions) { (float) width, (float) height }, 
                   (Clay_ErrorHandler) { HandleClayErrors });
    Clay_SetMeasureTextFunction(SDL_MeasureText, state->rendererData.fonts);

    state->demoData = ClayVideoDemo_Initialize();

    state->graphState = (GraphState) {
        .viewport = {
            .cx = 0.0,
            .cy = 0.0,
            .xScale = 50.0,
            .yScale = 50.0
        },
        .velocity = {0, 0},
        .needs_update = true
    };

    /* Read function from command line */
    const char *func_arg = get_cmd_arg(argc, argv, "--func=");
    if (func_arg && func_arg[0] != '\0') {
        SDL_strlcpy(
            state->graphState.function,
            func_arg,
            sizeof(state->graphState.function)
        );
    } else {
        SDL_strlcpy(
            state->graphState.function,
            "x",
            sizeof(state->graphState.function)
        );
    }

    update_graph_texture(state, width - 32, height - 150);

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    AppState *state = appstate;

    switch (event->type) {
        case SDL_EVENT_QUIT:
            return SDL_APP_SUCCESS;
            
        case SDL_EVENT_KEY_UP:
            if (event->key.scancode == SDL_SCANCODE_SPACE) {
                show_demo = !show_demo;
                if (!show_demo) show_graph = !show_graph;
            }
            break;
            
        case SDL_EVENT_WINDOW_RESIZED:
            Clay_SetLayoutDimensions((Clay_Dimensions) { 
                (float) event->window.data1, 
                (float) event->window.data2 
            });
            state->graphState.needs_update = true;
            break;
            
        case SDL_EVENT_MOUSE_MOTION:
            Clay_SetPointerState((Clay_Vector2) { event->motion.x, event->motion.y },
                                 event->motion.state & SDL_BUTTON_LMASK);
            break;
            
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            Clay_SetPointerState((Clay_Vector2) { event->button.x, event->button.y },
                                 event->button.button == SDL_BUTTON_LEFT);
            break;
            
        case SDL_EVENT_MOUSE_WHEEL:
            Clay_UpdateScrollContainers(true, (Clay_Vector2) { event->wheel.x, event->wheel.y }, 0.01f);
            break;
            
        default:
            break;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    AppState *state = appstate;
    
    static Uint64 last = 0;
    Uint64 now = SDL_GetTicks();
    double dt = (now - last) / 1000.0;
    if (dt > 0.1) dt = 0.1;
    last = now;

    // Update graph controls if showing graph
    if (show_graph) {
        update_graph_movement(&state->graphState, dt);
        update_graph_zoom(&state->graphState, dt);

        if (state->graphState.needs_update) {
            int width, height;
            SDL_GetWindowSize(state->window, &width, &height);
            update_graph_texture(state, width - 32, height - 150);
        }
    }

    // Render
    Clay_RenderCommandArray render_commands;
    if (show_demo) {
        render_commands = ClayVideoDemo_CreateLayout(&state->demoData);
    } else if (show_graph) {
        render_commands = ClayGraph_CreateLayout(state);
    } else {
        render_commands = ClayImageSample_CreateLayout(state);
    }

    SDL_SetRenderDrawColor(state->rendererData.renderer, 0, 0, 0, 255);
    SDL_RenderClear(state->rendererData.renderer);

    SDL_Clay_RenderClayCommands(&state->rendererData, &render_commands);

    SDL_RenderPresent(state->rendererData.renderer);

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    if (result != SDL_APP_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Application failed to run");
    }

    AppState *state = appstate;

    if (sample_image) {
        SDL_DestroyTexture(sample_image);
    }

    if (state) {
        if (state->graphState.graph_texture) {
            SDL_DestroyTexture(state->graphState.graph_texture);
        }

        if (state->rendererData.renderer)
            SDL_DestroyRenderer(state->rendererData.renderer);

        if (state->window)
            SDL_DestroyWindow(state->window);

        if (state->rendererData.fonts) {
            for(size_t i = 0; i < sizeof(state->rendererData.fonts) / sizeof(*state->rendererData.fonts); i++) {
                TTF_CloseFont(state->rendererData.fonts[i]);
            }
            SDL_free(state->rendererData.fonts);
        }

        if (state->rendererData.textEngine)
            TTF_DestroyRendererTextEngine(state->rendererData.textEngine);

        SDL_free(state);
    }
    TTF_Quit();
}