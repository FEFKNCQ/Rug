#pragma once

#ifdef RUG_EXPORTS
#define RUG_API __declspec(dllexport)
#else
#define RUG_API __declspec(dllimport)
#endif

extern "C" {

RUG_API int  rug_init(int width, int height);

RUG_API int  rug_load_texture(const wchar_t* path);

RUG_API void rug_rebuild(float scale, float drop);

RUG_API void rug_set_bumps(const float* data, int count);

RUG_API void rug_step(float dt);

RUG_API void rug_render(unsigned char* dst, int stride, int* rect);

RUG_API int  rug_mouse_down(float x, float y);
RUG_API void rug_mouse_drag(float x, float y);
RUG_API void rug_mouse_up(void);

RUG_API int  rug_hit_test(float x, float y);

RUG_API void rug_get_bounds(float* out);

RUG_API void rug_rotate(float angle);

RUG_API void rug_resize(float scale);

RUG_API void rug_get_pose(float* out);

RUG_API void rug_get_corners(float* out);

RUG_API void rug_hold_begin(void);
RUG_API void rug_hold_end(void);

RUG_API void rug_layout(float cx, float cy, float angle, float drop);

RUG_API void rug_shutdown(void);

}
