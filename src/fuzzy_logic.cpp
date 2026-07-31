#include "fuzzy_logic.h"

// Physical memory allocation for extern variables
float g_fisInput[3];
float g_fisOutput[1];

const int fis_gcI = 3; 
const int fis_gcO = 1; 
const int fis_gcR = 18; 

// ==========================================
// BUILT-IN MEMBERSHIP FUNCTIONS
// ==========================================
float fis_trapmf(float x, float* p) {
    float a = p[0], b = p[1], c = p[2], d = p[3];
    if (x <= a || x >= d) return 0.0;
    if (x >= b && x <= c) return 1.0;
    if (x > a && x < b) return (x - a) / (b - a);
    return (d - x) / (d - c);
}

float fis_trimf(float x, float* p) {
    float a = p[0], b = p[1], c = p[2];
    if (x <= a || x >= c) return 0.0;
    if (x == b) return 1.0;
    if (x < b) return (x - a) / (b - a);
    return (c - x) / (c - b);
}

float fis_min(float a, float b) { return (a < b) ? a : b; }

// Function pointer array for dynamic MF selection (0: trap, 1: tri)
typedef float(*_FIS_MF)(float, float*);
_FIS_MF fis_gMF[] = { fis_trapmf, fis_trimf }; 

// ==========================================
// FUZZY PARAMETERS (SHOULDER CURVES ENABLED)
// ==========================================
// 1. Voltage
float fis_gMFICoeff_q0[] = { -1000.0, -1000.0, 198.0, 200.0 }; // Under
float fis_gMFICoeff_q1[] = { 198.0, 220.0, 242.0 };            // Normal (Triangle)
float fis_gMFICoeff_q2[] = { 240.0, 242.0, 1000.0, 1000.0 };   // Over
float* fis_gMFICoeff_0[] = { fis_gMFICoeff_q0, fis_gMFICoeff_q1, fis_gMFICoeff_q2 };

// 2. Frequency
float fis_gMFICoeff_q3[] = { -1000.0, -1000.0, 49.50, 49.60 }; // Under
float fis_gMFICoeff_q4[] = { 49.50, 49.60, 50.40, 50.50 };     // Normal (Trapezoid)
float fis_gMFICoeff_q5[] = { 50.40, 50.50, 1000.0, 1000.0 };   // Over
float* fis_gMFICoeff_1[] = { fis_gMFICoeff_q3, fis_gMFICoeff_q4, fis_gMFICoeff_q5 };

// 3. Duration
float fis_gMFICoeff_q6[] = { -1000.0, -1000.0, 55.0, 65.0 };   // Short
float fis_gMFICoeff_q7[] = { 55.0, 65.0, 1000.0, 1000.0 };     // Long
float* fis_gMFICoeff_2[] = { fis_gMFICoeff_q6, fis_gMFICoeff_q7 };

float** fis_gMFICoeff[] = { fis_gMFICoeff_0, fis_gMFICoeff_1, fis_gMFICoeff_2 };

// Sugeno Output Constants (0.0 = PV/Solar, 1.0 = Grid)
float fis_gMFOCoeff_q0[] = { 0.0 }; 
float fis_gMFOCoeff_q1[] = { 1.0 }; 
float* fis_gMFOCoeff_0[] = { fis_gMFOCoeff_q0, fis_gMFOCoeff_q1 };
float** fis_gMFOCoeff[] = { fis_gMFOCoeff_0 };

int fis_gIMFCount[] = { 3, 3, 2 };

// Membership Function Types (0: trapmf, 1: trimf)
int fis_gMFI_0[] = { 0, 1, 0 }; // Voltage: Trap, Tri, Trap
int fis_gMFI_1[] = { 0, 0, 0 }; // Frequency: Trap, Trap, Trap
int fis_gMFI_2[] = { 0, 0 };    // Time: Trap, Trap
int* fis_gMFI[] = { fis_gMFI_0, fis_gMFI_1, fis_gMFI_2 };

// Rule Input Matrix (1-based index)
int fis_gRI[18][3] = {
    {2, 2, 1}, {2, 2, 2}, {2, 1, 1}, {2, 1, 2}, {2, 3, 1}, {2, 3, 2},
    {1, 2, 1}, {1, 2, 2}, {1, 1, 1}, {1, 1, 2}, {1, 3, 1}, {1, 3, 2},
    {3, 2, 1}, {3, 2, 2}, {3, 1, 1}, {3, 1, 2}, {3, 3, 1}, {3, 3, 2}
};

// ==========================================
// DECISION MATRICES (1 = PV/Solar, 2 = Grid)
// ==========================================
int fis_gRO_Production[18][1] = { {1}, {1}, {1}, {2}, {1}, {2}, {1}, {2}, {1}, {2}, {2}, {2}, {1}, {2}, {1}, {2}, {1}, {2} };
int fis_gRO_Transition[18][1] = { {1}, {1}, {2}, {2}, {2}, {2}, {2}, {2}, {2}, {2}, {2}, {2}, {2}, {2}, {2}, {2}, {2}, {2} };
int fis_gRO_NonProduction[18][1] = { {2}, {2}, {2}, {1}, {2}, {1}, {2}, {1}, {2}, {1}, {2}, {1}, {2}, {1}, {2}, {1}, {2}, {1} };

// ==========================================
// SUGENO CORE ENGINE
// ==========================================
void fis_evaluate_core(int period) {
    float fuzzyFires[18] = {0};
    float sW = 0;

    int (*active_RO)[1];
    if (period == 0) active_RO = fis_gRO_Production;
    else if (period == 1) active_RO = fis_gRO_Transition;
    else active_RO = fis_gRO_NonProduction;

    // Rule Evaluation (AND Logic)
    for (int r = 0; r < fis_gcR; ++r) {
        fuzzyFires[r] = 1.0;
        for (int i = 0; i < fis_gcI; ++i) {
            int index = fis_gRI[r][i] - 1;
            int mfType = fis_gMFI[i][index]; 
            float mfValue = (fis_gMF[mfType])(g_fisInput[i], fis_gMFICoeff[i][index]);
            fuzzyFires[r] = fis_min(fuzzyFires[r], mfValue);
        }
        sW += fuzzyFires[r];
    }

    // Defuzzification (Weighted Average)
    if (sW == 0) {
        g_fisOutput[0] = 1.0; // Fail-safe default to Grid (PLN)
    } else {
        float weightedSum = 0;
        for (int r = 0; r < fis_gcR; ++r) {
            int outputIndex = active_RO[r][0] - 1; 
            float sugenoConstant = fis_gMFOCoeff[0][outputIndex][0];
            weightedSum += (fuzzyFires[r] * sugenoConstant);
        }
        g_fisOutput[0] = weightedSum / sW;
    }
}

// Wrapper Functions
void fis_evaluate_Production() { fis_evaluate_core(0); }
void fis_evaluate_Transition() { fis_evaluate_core(1); }
void fis_evaluate_NonProduction() { fis_evaluate_core(2); }