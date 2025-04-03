#include <iostream>
#include <cstring>
#include <unistd.h>
#include "Palettes.h"
#include "LeptonThread.h"

void printUsage(char *cmd) {
    char *cmdname = basename(cmd);
    printf("Usage: %s [OPTION]...\n"
           " -h      display this help and exit\n"
           " -cm x   select colormap\n"
           "           1 : rainbow\n"
           "           2 : grayscale\n"
           "           3 : ironblack [default]\n"
           " -tl x   select type of Lepton\n"
           "           2 : Lepton 2.x [default]\n"
           "           3 : Lepton 3.x\n"
           " -ss x   SPI bus speed [MHz] (10 - 30)\n"
           "           20 : 20MHz [default]\n"
           " -min x  override minimum value for scaling (0 - 65535)\n"
           " -max x  override maximum value for scaling (0 - 65535)\n"
           " -d x    log level (0-255)\n"
           "", cmdname);
    return;
}

int main(int argc, char **argv) {
    int typeColormap = 3;  // 기본 컬러맵: IronBlack
    int typeLepton = 2;     // 기본 Lepton 버전: 2.x
    int spiSpeed = 20;      // 기본 SPI 속도: 20MHz
    int rangeMin = -1;      
    int rangeMax = -1;
    int loglevel = 0;
    //target temperature
    int sigMin = -1;
    int sigMax = -1;

    // 🔹 명령줄 인자 파싱
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "-d") == 0 && (i + 1 != argc)) {
            loglevel = std::atoi(argv[i + 1]) & 0xFF;
            i++;
        } else if (strcmp(argv[i], "-cm") == 0 && (i + 1 != argc)) {
            int val = std::atoi(argv[i + 1]);
            if (val == 1 || val == 2) {
                typeColormap = val;
                i++;
            }
        } else if (strcmp(argv[i], "-tl") == 0 && (i + 1 != argc)) {
            int val = std::atoi(argv[i + 1]);
            if (val == 3) {
                typeLepton = val;
                i++;
            }
        } else if (strcmp(argv[i], "-ss") == 0 && (i + 1 != argc)) {
            int val = std::atoi(argv[i + 1]);
            if (10 <= val && val <= 30) {
                spiSpeed = val;
                i++;
            }
        } else if (strcmp(argv[i], "-min") == 0 && (i + 1 != argc)) {
            int val = std::atoi(argv[i + 1]);
            if (0 <= val && val <= 65535) {
                rangeMin = val;
                i++;
            }
        } else if (strcmp(argv[i], "-max") == 0 && (i + 1 != argc)) {
            int val = std::atoi(argv[i + 1]);
            if (0 <= val && val <= 65535) {
                rangeMax = val;
                i++;
            }
        }else if (strcmp(argv[i], "-sigmin") == 0 && (i + 1 != argc)) {
            int val = std::atoi(argv[i + 1]);
            if (0 <= val && val <= 65535) {
                sigMin = val;
                i++;
            }
        }else if (strcmp(argv[i], "-sigmax") == 0 && (i + 1 != argc)) {
            int val = std::atoi(argv[i + 1]);
            if (0 <= val && val <= 65535) {
                sigMax = val;
                i++;
            }
        }

    }


    LeptonThread *thread = new LeptonThread();
    thread->setLogLevel(loglevel);
    // sigMin과 sigMax으로 PaletteCustomizing
    if (sigMin >= 0 && sigMax >= 0) customizePalette(sigMin, sigMax);
    else customizePalette();
    //
    thread->useColormap(typeColormap);
    thread->useLepton(typeLepton);
    thread->useSpiSpeedMhz(spiSpeed);
    thread->setAutomaticScalingRange();
    if (rangeMin >= 0) thread->useRangeMinValue(rangeMin);
    if (rangeMax >= 0) thread->useRangeMaxValue(rangeMax);
    
    
    thread->start();

    
    while (true) {
        usleep(100000);  
    }

    return 0;
}

