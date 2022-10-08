/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2022 Xilinx, Inc. */

#ifndef _SFPTPD_APP_H
#define _SFPTPD_APP_H

#include "sfptpd_message.h"


/****************************************************************************
 * Structures and Types
 ****************************************************************************/


/****************************************************************************
 * Function Prototypes
 ****************************************************************************/


/** Send a message to the thread of an application component to indicate the
 * parent is ready for normal operation to begin.
 * @param component the target component
 */
void sfptpd_app_run(struct sfptpd_thread *component);


/****************************************************************************
 * Application Messages
 *
 * These are generic messages to enable the operation of an application but
 * which are neither part of the threading basics nor the
 * application-specificspecialised responsibilities of the engine.
 ****************************************************************************/

/** Macro used to define message ID values for application messages */
#define SFPTPD_APP_MSG(x) (SFPTPD_MSG_BASE_APP + (x))

/** Message to indicate that the parent component's thread has started up
 * and this thread many now begin normal operation, such as starting timers
 * and initiating other activity that could result in messages to the parent.
 * There is no payload and no reply.
 */
#define SFPTPD_APP_MSG_RUN SFPTPD_APP_MSG(0)


#endif /* _SFPTPD_APP_H */
