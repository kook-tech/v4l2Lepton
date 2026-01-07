
TEMPLATE = app
QT += core gui
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = raspberrypi_video

RPI_LIBS = ../raspberrypi_libs
LEPTONSDK = leptonSDKEmb32PUB

PRE_TARGETDEPS += sdk
QMAKE_EXTRA_TARGETS += sdk sdkclean
sdk.commands = make -C $${RPI_LIBS}/$${LEPTONSDK}
sdkclean.commands = make -C $${RPI_LIBS}/$${LEPTONSDK} clean

DEPENDPATH += .
INCLUDEPATH += . $${RPI_LIBS}

# 컴파일러 최적화 옵션
QMAKE_CXXFLAGS += -O3 -march=native -mtune=native
QMAKE_CFLAGS += -O3 -march=native -mtune=native

DESTDIR=.
OBJECTS_DIR=gen_objs
MOC_DIR=gen_mocs

HEADERS += *.h

SOURCES += *.cpp

unix:LIBS += -L$${RPI_LIBS}/$${LEPTONSDK}/Debug -lLEPTON_SDK

# MQTT capture command/response
unix:LIBS += -lmosquitto

unix:QMAKE_CLEAN += -r $(OBJECTS_DIR) $${MOC_DIR}

