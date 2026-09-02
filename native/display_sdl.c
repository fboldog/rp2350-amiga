#include "display_sdl.h"
#include <SDL2/SDL.h>
#include <stdio.h>

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;
static int           tw, th;
static uint32_t      last_push_ms;

void sdl_display_open(int w, int h) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");   /* nearest-neighbour */
    win = SDL_CreateWindow("Omega Amiga",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w * 2, h * 2,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return; }
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!tex) { fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); return; }
    tw = w; th = h;
    last_push_ms = 0;
}

void sdl_display_push(const uint32_t *argb32) {
    if (!tex) return;
    uint32_t now = SDL_GetTicks();
    if (now - last_push_ms < 33) return;   /* ~30 fps display cap */
    last_push_ms = now;
    SDL_UpdateTexture(tex, NULL, argb32, tw * (int)sizeof(uint32_t));
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

int sdl_display_poll(void) {
    if (!win) return 0;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) return 1;
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) return 1;
    }
    return 0;
}

void sdl_display_close(void) {
    if (tex) { SDL_DestroyTexture(tex);  tex = NULL; }
    if (ren) { SDL_DestroyRenderer(ren); ren = NULL; }
    if (win) { SDL_DestroyWindow(win);   win = NULL; }
    SDL_Quit();
}
