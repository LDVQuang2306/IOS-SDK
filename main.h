//
//  main.h
//  Dumper
//
//  Created by Euclid Jan Guillermo on 12/8/25.
//

#pragma once

/* Starts the SDK generation on a background thread (non-blocking). Ignored while a dump is running or after a finished dump. */
void StartDump();

bool IsDumpRunning();
