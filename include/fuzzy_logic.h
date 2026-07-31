#pragma once
#include "config.h"

// External arrays for Fuzzy Input and Output
extern float g_fisInput[3];
extern float g_fisOutput[1];

// Evaluator functions based on time zones
void fis_evaluate_Production();
void fis_evaluate_Transition();
void fis_evaluate_NonProduction();