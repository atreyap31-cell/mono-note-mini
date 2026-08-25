#ifndef PALA_RECORD_H
#define PALA_RECORD_H
#include <Arduino.h>

bool recBegin();
void recPoll();
uint32_t recSeconds();
bool recSave(const String& wavPath);
void recDiscard();

#endif
