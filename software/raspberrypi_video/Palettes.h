#ifndef PALETTES_H
#define PALETTES_H

extern const int colormap_rainbow[];
extern const int colormap_grayscale[];
extern int colormap_ironblack[];
extern int get_size_colormap_rainbow();
extern int get_size_colormap_grayscale();
extern int get_size_colormap_ironblack();
//
extern void customizePalette(int sigMin, int sigMax, int rangeMin, int rangeMax);
extern void customizePalette();
extern void exportColormapToCSV(int* colormap, int size);
#endif
