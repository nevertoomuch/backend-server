#pragma once
#include <libpq-fe.h>

void RunGUI();
void LoadDataFromDB(PGconn* con);
void ColoredIndicator(const char* label, bool condition,
                     const char* true_text = "ON", const char* false_text = "OFF");