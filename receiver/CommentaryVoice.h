#pragma once

#include <cstddef>

void CommentaryVoice_Init();
void CommentaryVoice_Queue(const char* text, int priority, unsigned long long sequence);
void CommentaryVoice_Reset();

