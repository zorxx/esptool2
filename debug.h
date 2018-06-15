#ifndef _DEBUG_H
#define _DEBUG_H

#include <stdint.h>

extern uint8_t debug_level;
#define DEBUG(...) if(debug_level >= 3) printf(__VA_ARGS__) 
#define PRINT(...) if(debug_level >= 2) printf(__VA_ARGS__) 
#define ERROR(...) if(debug_level >= 1) printf(__VA_ARGS__) 

#endif /* _DEBUG_H */

