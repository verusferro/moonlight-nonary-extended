TEMPLATE = app
TARGET = tst_chordtracker
CONFIG += console c++17
CONFIG -= qt app_bundle
INCLUDEPATH += $$PWD/../../app/streaming/input
SOURCES += $$PWD/tst_chordtracker.cpp
