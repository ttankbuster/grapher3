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
    int mouseX, mouseY;
    bool mouse_in_window;
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

static inline Vec2d screen_to_math(const Viewport *v, int sx, int sy, int width, int height)
{
    Vec2d p;
    p.x = v->cx + (sx - width / 2.0) / v->xScale;
    p.y = v->cy - (sy - height / 2.0) / v->yScale;
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

double numerical_derivative(const char *func, double x0)
{
    const double h = 1e-7;
    
    double x = x0;
    te_variable vars[] = {{"x", &x}};
    int err;
    
    char *expanded = expand_implicit_mul(func);
    te_expr *expr = te_compile(expanded, vars, 1, &err);
    free(expanded);
    
    if (!expr || err) {
        return NAN;
    }
    
    x = x0 + h;
    double f_plus = te_eval(expr);
    
    x = x0 - h;
    double f_minus = te_eval(expr);
    
    te_free(expr);
    
    return (f_plus - f_minus) / (2.0 * h);
}

int draw_tangent(SDL_Renderer* renderer, const Viewport *v, const char* func, 
                 int mouseX, int mouseY, int width, int height) 
{
    // Account for UI padding (16px on each side from Clay layout)
    const int UI_PADDING = 16;
    const int UI_TOP_HEIGHT = 80;  // Approximate height of title and instructions
    
    // Adjust mouse coordinates to graph coordinate space
    int graph_mouse_x = mouseX - UI_PADDING;
    int graph_mouse_y = mouseY - UI_PADDING - UI_TOP_HEIGHT;
    int graph_width = width - 2 * UI_PADDING;
    int graph_height = height - 2 * UI_PADDING - UI_TOP_HEIGHT - 40;  // 40 for bottom text
    
    // Check if mouse is within graph bounds
    if (graph_mouse_x < 0 || graph_mouse_x >= graph_width ||
        graph_mouse_y < 0 || graph_mouse_y >= graph_height) {
        return 0;
    }
    
    // Convert screen coords to math coords
    Vec2d math_pos = screen_to_math(v, graph_mouse_x, graph_mouse_y, graph_width, graph_height);
    double x0 = math_pos.x;
    
    // Evaluate function at x0
    double x = x0;
    te_variable vars[] = {{"x", &x}};
    int err;
    
    char *expanded = expand_implicit_mul(func);
    te_expr *expr = te_compile(expanded, vars, 1, &err);
    free(expanded);
    
    if (!expr || err) {
        return -1;
    }
    
    double y0 = te_eval(expr);
    te_free(expr);
    
    if (!isfinite(y0)) {
        return 0;
    }
    
    // Calculate derivative (slope)
    double slope = numerical_derivative(func, x0);
    
    if (!isfinite(slope)) {
        return 0;
    }
    
    // Draw tangent line across visible area
    double hw = (graph_width / 2.0) / v->xScale;
    double x_left = v->cx - hw;
    double x_right = v->cx + hw;
    
    double y_left = y0 + slope * (x_left - x0);
    double y_right = y0 + slope * (x_right - x0);
    
    SDL_FPoint p1 = math_to_screen(v, x_left, y_left, graph_width, graph_height);
    SDL_FPoint p2 = math_to_screen(v, x_right, y_right, graph_width, graph_height);
    
    // Adjust back to window coordinates
    p1.x += UI_PADDING;
    p1.y += UI_PADDING + UI_TOP_HEIGHT;
    p2.x += UI_PADDING;
    p2.y += UI_PADDING + UI_TOP_HEIGHT;
    
    // Draw tangent line in red
    SDL_SetRenderDrawColor(renderer, 255, 50, 50, 255);
    SDL_RenderLines(renderer, (SDL_FPoint[]){p1, p2}, 2);
    
    // Draw point on curve
    SDL_FPoint point = math_to_screen(v, x0, y0, graph_width, graph_height);
    point.x += UI_PADDING;
    point.y += UI_PADDING + UI_TOP_HEIGHT;
    
    SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255);
    const float radius = 4.0f;
    for (int i = -3; i <= 3; i++) {
        for (int j = -3; j <= 3; j++) {
            if (i*i + j*j <= 9) {
                SDL_RenderPoint(renderer, point.x + i, point.y + j);
            }
        }
    }
    
    return 0;
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

    SDL_Surface *surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        SDL_Log("Failed to create surface: %s", SDL_GetError());
        return NULL;
    }

    SDL_Renderer *soft_renderer = SDL_CreateSoftwareRenderer(surface);
    if (!soft_renderer) {
        SDL_Log("Failed to create software renderer: %s", SDL_GetError());
        SDL_DestroySurface(surface);
        return NULL;
    }

    SDL_SetRenderDrawColor(soft_renderer, 0, 0, 0, 255);
    SDL_RenderClear(soft_renderer);

    draw_grid(soft_renderer, viewport, width, height);
    draw_axes(soft_renderer, viewport, width, height);

    SDL_SetRenderDrawColor(soft_renderer, 0, 255, 0, 255);
    drawGraph(soft_renderer, viewport, function, width, height);

    SDL_RenderPresent(soft_renderer);
    SDL_DestroyRenderer(soft_renderer);

    SDL_Color label_color = { 200, 200, 200, 255 };

    double step = grid_step(viewport->xScale);
    double halfW = (width / 2.0) / viewport->xScale;
    double halfH = (height / 2.0) / viewport->yScale;
    double xStart = floor((viewport->cx - halfW) / step) * step;
    double xEnd   = ceil ((viewport->cx + halfW) / step) * step;
    double yStart = floor((viewport->cy - halfH) / step) * step;
    double yEnd   = ceil ((viewport->cy + halfH) / step) * step;

    SDL_FPoint axis_origin = math_to_screen(viewport, 0.0, 0.0, width, height);
    int axis_y = (int)axis_origin.y;
    int axis_x = (int)axis_origin.x;

    const int pad = 4;

    for (double x = xStart; x <= xEnd; x += step) {
        if (fabs(x) < 1e-9) continue;

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

    for (double y = yStart; y <= yEnd; y += step) {
        if (fabs(y) < 1e-9) continue;

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

        CLAY_TEXT(CLAY_STRING("WASD to pan • Z/X to zoom • Space to toggle • Hover for tangent"), CLAY_TEXT_CONFIG({
            .fontId = FONT_ID,
            .fontSize = 16,
            .textColor = {100, 100, 100, 255}
        }));

        CLAY(CLAY_ID("GraphContainer"), {
            .layout = {
                .sizing = layoutExpand
            },
            .image = {
                .imageData = state->graphState.graph_texture,
            }
        });

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
        .needs_update = true,
        .mouseX = 0,
        .mouseY = 0,
        .mouse_in_window = false
    };

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
            "x^2",
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
            state->graphState.mouseX = event->motion.x;
            state->graphState.mouseY = event->motion.y;
            state->graphState.mouse_in_window = true;
            break;
            
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            Clay_SetPointerState((Clay_Vector2) { event->button.x, event->button.y },
                                 event->button.button == SDL_BUTTON_LEFT);
            state->graphState.mouseX = event->button.x;
            state->graphState.mouseY = event->button.y;
            break;
            
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            state->graphState.mouse_in_window = false;
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

    if (show_graph) {
        update_graph_movement(&state->graphState, dt);
        update_graph_zoom(&state->graphState, dt);

        if (state->graphState.needs_update) {
            int width, height;
            SDL_GetWindowSize(state->window, &width, &height);
            update_graph_texture(state, width - 32, height - 150);
        }
    }

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

    if (show_graph && state->graphState.mouse_in_window) {
        int width, height;
        SDL_GetWindowSize(state->window, &width, &height);
        draw_tangent(state->rendererData.renderer, 
                     &state->graphState.viewport,
                     state->graphState.function,
                     state->graphState.mouseX,
                     state->graphState.mouseY,
                     width, height);
    }

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