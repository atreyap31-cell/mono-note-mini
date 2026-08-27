#ifndef PALA_RECORD_H
#define PALA_RECORD_H
#include <Arduino.h>

bool recBegin();   /* capture then runs in its own task until recSave/recDiscard */
uint32_t recSeconds();
int recLevel();
bool recSave(const String& wavPath);
void recDiscard();

void audioReady();
void beep();
bool playFile(const String& wavPath);
void playPoll();
void playStop();
bool playActive();

#endif
