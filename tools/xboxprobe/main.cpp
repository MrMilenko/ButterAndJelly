// Minimal boot probe: our exact build settings, none of our subsystems.
// Each stage paints a colour, so the screen says how far it got with no
// debugger attached.
//
//   green   SDL_Init(video) returned
//   blue    window and renderer created
//   white   a std::string and a std::vector round-tripped (libc++ alive)
//   yellow  a thread ran and joined
//   red     something threw or failed

#include <SDL.h>
#include <string>
#include <vector>

static SDL_Renderer* g_renderer = 0;

static void paint(int r, int g, int b, int frames)
{
    for (int i = 0; i < frames; ++i) {
        SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);
        SDL_RenderClear(g_renderer);
        SDL_RenderPresent(g_renderer);
    }
}

static int SDLCALL Worker(void* data)
{
    int* flag = (int*)data;
    *flag = 1;
    return 0;
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;

    SDL_Window* window = SDL_CreateWindow("probe", 0, 0, 640, 480,
                                          SDL_WINDOW_FULLSCREEN);
    if (!window) return 1;
    g_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) return 1;

    paint(0, 200, 0, 120);          // green: SDL and a renderer are up

    paint(0, 100, 220, 120);        // blue: about to touch libc++

    {
        std::string s = "butter";
        s += " and jelly";
        std::vector<int> v;
        for (int i = 0; i < 100; ++i) v.push_back(i);
        if (s.size() != 16 || v.size() != 100) { paint(220, 0, 0, 600); return 1; }
    }
    paint(230, 230, 230, 120);      // white: libc++ works

    {
        int ran = 0;
        SDL_Thread* t = SDL_CreateThread(Worker, "probe", &ran);
        if (!t) { paint(220, 0, 0, 600); return 1; }
        SDL_WaitThread(t, 0);
        if (!ran) { paint(220, 0, 0, 600); return 1; }
    }
    paint(220, 200, 0, 600);        // yellow: threads work, and we survived

    SDL_Quit();
    return 0;
}
