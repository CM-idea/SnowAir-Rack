#pragma once

#include "Common.h"

/*
 * 便签配色。
 *
 * 编辑器（NoteEditor）和绳子上的卡片（RackView）都要画便签，
 * 之前各写了一套一模一样的色值，改一处就得记得改另一处；
 * 现在统一放在这里，两边都从这里取。
 */
struct NotePalette {
    D2D1_COLOR_F fill; /* 纸色 */
    D2D1_COLOR_F ink;  /* 正文 */
    D2D1_COLOR_F edge; /* 描边 */
    D2D1_COLOR_F fold; /* 右下折角 */
};

/* 可选的三种便签底色（color 取 0..2），深/浅色主题各一套。 */
inline NotePalette NotePaletteFor(int color, bool dark)
{
    switch (color % 3) {
    case 1:
        return dark ? NotePalette{D2D1::ColorF(0.36f, 0.52f, 0.44f), D2D1::ColorF(0.93f, 0.97f, 0.94f),
                                   D2D1::ColorF(0.27f, 0.42f, 0.35f), D2D1::ColorF(0.28f, 0.43f, 0.36f)}
                    : NotePalette{D2D1::ColorF(0.81f, 0.95f, 0.87f), D2D1::ColorF(0.08f, 0.31f, 0.23f),
                                  D2D1::ColorF(0.66f, 0.87f, 0.76f), D2D1::ColorF(0.70f, 0.89f, 0.80f)};
    case 2:
        return dark ? NotePalette{D2D1::ColorF(0.47f, 0.43f, 0.62f), D2D1::ColorF(0.95f, 0.94f, 0.99f),
                                   D2D1::ColorF(0.36f, 0.32f, 0.50f), D2D1::ColorF(0.38f, 0.34f, 0.52f)}
                    : NotePalette{D2D1::ColorF(0.89f, 0.86f, 0.98f), D2D1::ColorF(0.23f, 0.17f, 0.39f),
                                  D2D1::ColorF(0.79f, 0.75f, 0.93f), D2D1::ColorF(0.82f, 0.78f, 0.94f)};
    default:
        return dark ? NotePalette{D2D1::ColorF(0.62f, 0.55f, 0.34f), D2D1::ColorF(0.99f, 0.96f, 0.85f),
                                   D2D1::ColorF(0.48f, 0.42f, 0.24f), D2D1::ColorF(0.50f, 0.44f, 0.26f)}
                    : NotePalette{D2D1::ColorF(1.0f, 0.945f, 0.66f), D2D1::ColorF(0.35f, 0.29f, 0.07f),
                                  D2D1::ColorF(0.91f, 0.84f, 0.48f), D2D1::ColorF(0.93f, 0.86f, 0.53f)};
    }
}

/*
 * 纯便签卡片（整张卡就是一张纸）用的黄色纸张：
 * 斜向渐变 + 深棕墨色，编辑器的便签形态也用这一组，交接时才对得上。
 */
inline D2D1_COLOR_F NotePaperTop()
{
    return D2D1::ColorF(0xF4D476);
}

inline D2D1_COLOR_F NotePaperBottom()
{
    return D2D1::ColorF(0xF7E3A7);
}

inline D2D1_COLOR_F NotePaperEdge()
{
    return D2D1::ColorF(0xE0C97E);
}

inline D2D1_COLOR_F NotePaperInk()
{
    return D2D1::ColorF(0x5A462A);
}
