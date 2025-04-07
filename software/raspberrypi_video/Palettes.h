#ifndef PALETTES_H
#define PALETTES_H

extern const int colormap_rainbow[];
extern const int colormap_grayscale[];
extern int colormap_ironblack[];
//개발
extern int colormap_custom[];
//


extern int get_size_colormap_rainbow();
extern int get_size_colormap_grayscale();
extern int get_size_colormap_ironblack();

//개발
extern int get_size_colormap_custom();
extern void customizePalette(int sigMin, int sigMax, int rangeMin, int rangeMax);
extern void customizePalette2(int sigMin, int sigMax, int rangeMin, int rangeMax);
extern void exportColormapToCSV(int* colormap, int size);
//미구현
extern void customizePalette();
#endif
