/* Native C++ computer opponent for the Amiga 68k port.
 *
 * Why this exists: the stock 1.0.5 AI is Squirrel, and its AI_VMSuspend
 * exception cannot unwind through the VM in this port's Hunk executable, so any
 * Squirrel AI aborts the game. This subsystem is plain C++ that issues commands
 * through the native DoCommandP - which throws nothing - so it sidesteps the
 * whole problem. It is wired in place of AI::StartNew / AI::GameLoop for AI
 * companies; the Squirrel VM is never started for them.
 */
#ifndef OLDAI_H
#define OLDAI_H

#include "../company_type.h"

void OldAI_Initialize();
void OldAI_Start(CompanyID company);
void OldAI_CompanyDied(CompanyID company);
void OldAI_GameLoop();

#endif /* OLDAI_H */
