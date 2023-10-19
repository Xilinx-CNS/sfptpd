/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_test_bic.c
 * @brief  Best Instance Clock unit tests
 */
#include <math.h>
#include <errno.h>
#include "sfptpd_bic.h"

/****************************************************************************
 * External declarations
 ****************************************************************************/

/****************************************************************************
 * Types and Defines
 ****************************************************************************/

#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof (a [0]))

/****************************************************************************
 * Local Data
 ****************************************************************************/

/* Single instance in slave state */
struct sync_instance_record  single_slave [] =
{
	{
		{
			NULL,
			NULL,
			"A"
		},
		{
			SYNC_MODULE_STATE_SLAVE,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1.0,
				1.0,
				2
			},
			0.0
		}
	}
};


/* Single instance with alarm raised */
struct sync_instance_record  single_alarmed [] =
{
	{
		{
			NULL,
			NULL,
			"A"
		},
		{
			SYNC_MODULE_STATE_SLAVE,
			SYNC_MODULE_ALARM_NO_FOLLOW_UPS,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1.0,
				1.0,
				2
			},
			0.0
		}
	}
};


/* Single instance in non-slave state */
struct sync_instance_record  single_non_slave [] =
{
	{
		{
			NULL,
			NULL,
			"non-slave"
		},
		{
			SYNC_MODULE_STATE_LISTENING,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_FREERUNNING,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				INFINITY,
				0,
				0
			},
			0.0
		}
	}
};

/* Two instances, second better */
struct sync_instance_record  two [] =
{
	{
		{
			NULL,
			NULL,
			"A"
		},
		{
			SYNC_MODULE_STATE_SLAVE,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1.0,
				1.0,
				2
			},
			0.0
		}
	},
	{
		{
			NULL,
			NULL,
			"B"
		},
		{
			SYNC_MODULE_STATE_SLAVE,
			0,
			0,
			NULL,
			{ 0, 0 },
			32,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1.0,
				1.0,
				2
			},
			0.0
		}
	}
};

/* Two instances, second better but alarmed */
struct sync_instance_record  two_alarmed [] =
{
	{
		{
			NULL,
			NULL,
			"A"
		},
		{
			SYNC_MODULE_STATE_SLAVE,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1.0,
				1.0,
				2
			},
			0.0
		}
	},
	{
		{
			NULL,
			NULL,
			"B"
		},
		{
			SYNC_MODULE_STATE_SLAVE,
			SYNC_MODULE_ALARM_NO_FOLLOW_UPS,
			0,
			NULL,
			{ 0, 0 },
			32,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1.0,
				1.0,
				2
			},
			0.0
		}
	}
};


/* Two instances, second better all alarmed */
struct sync_instance_record  two_all_alarmed [] =
{
	{
		{
			NULL,
			NULL,
			"A"
		},
		{
			SYNC_MODULE_STATE_SLAVE,
			SYNC_MODULE_ALARM_NO_FOLLOW_UPS,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1.0,
				1.0,
				2
			},
			0.0
		}
	},
	{
		{
			NULL,
			NULL,
			"B"
		},
		{
			SYNC_MODULE_STATE_SLAVE,
			SYNC_MODULE_ALARM_NO_FOLLOW_UPS,
			0,
			NULL,
			{ 0, 0 },
			32,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1.0,
				1.0,
				2
			},
			0.0
		}
	}
};


/* Two instances, first better by virtue of steps removed*/
struct sync_instance_record  two_stepped [] =
{
	{
		{
			NULL,
			NULL,
			"A"
		},
		{
			SYNC_MODULE_STATE_SLAVE,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1.0,
				1.0,
				2
			},
			0.0
		}
	},
	{
		{
			NULL,
			NULL,
			"B"
		},
		{
			SYNC_MODULE_STATE_SLAVE,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1.0,
				1.0,
				5
			},
			0.0
		}
	}
};

/* Two instances, first non-slave and second slave */
struct sync_instance_record two_slave_and_non_slave[] =
{
	{
		{
			NULL,
			NULL,
			"A"
		},
		{
			SYNC_MODULE_STATE_LISTENING,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1.0,
				1.0,
				0
			},
			0.0
		}
	},
	{
		{
			NULL,
			NULL,
			"B"
		},
		{
			SYNC_MODULE_STATE_SLAVE,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_FREERUNNING,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1000000.0,
				1.0,
				5
			},
			500000.0
		}
	}
};

/* Two instances, both non-slaves in different states */
struct sync_instance_record two_non_slave_diff[] =
{
	{
		{
			NULL,
			NULL,
			"A"
		},
		{
			SYNC_MODULE_STATE_FAULTY,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_FREERUNNING,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1.0,
				1.0,
				0
			},
			0.0
		}
	},
	{
		{
			NULL,
			NULL,
			"B"
		},
		{
			SYNC_MODULE_STATE_PASSIVE,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_FREERUNNING,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				1.0,
				1.0,
				0
			},
			500000.0
		}
	}
};

/* Two instances, both non-slaves in same state */
struct sync_instance_record two_non_slave_same[] =
{
	{
		{
			NULL,
			NULL,
			"A"
		},
		{
			SYNC_MODULE_STATE_MASTER,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				100.0,
				1.0,
				0
			},
			0.0
		}
	},
	{
		{
			NULL,
			NULL,
			"B"
		},
		{
			SYNC_MODULE_STATE_MASTER,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				200.0,
				1.0,
				0
			},
			500000.0
		}
	}
};

/* Two instances, first PTP and second NTP reporting better accuracy */
struct sync_instance_record  ptp_ntp [] =
{
	{
		{
			NULL,
			NULL,
			"PTP"
		},
		{
			SYNC_MODULE_STATE_SLAVE,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				10000.0,
				1.0,
				2
			},
			SFPTPD_ACCURACY_PTP_HW
		}
	},
	{
		{
			NULL,
			NULL,
			"NTP"
		},
		{
			SYNC_MODULE_STATE_SLAVE,
			0,
			0,
			NULL,
			{ 0, 0 },
			64,
			{
				{},
				true,
				SFPTPD_CLOCK_CLASS_LOCKED,
				SFPTPD_TIME_SOURCE_ATOMIC_CLOCK,
				9000.00,
				1.0,
				2
			},
			SFPTPD_ACCURACY_NTP
		}
	}
};


/****************************************************************************
 * Local Functions
 ****************************************************************************/

int test_select (char *name, struct sync_instance_record *statuses, size_t num_tests, int expected)
{
	struct sync_instance_record *result;
	bool passed;

	result = sfptpd_bic_choose (&sfptpd_default_selection_policy, statuses, num_tests, NULL);
        passed = ((expected == -1) && (result == NULL)) ||
                 ((expected != -1) && (result == &statuses [expected]));
	printf ("SELECT %s: E %2d - %s\n", passed ? "PASS" : "FAIL", expected, name);

	return !passed;
}

/****************************************************************************
 * Entry Point
 ****************************************************************************/

int sfptpd_test_bic (void)
{
	int rc = 0;

	rc += test_select ("Single slave", single_slave, ARRAY_SIZE(single_slave), 0);
	rc += test_select ("Single alarmed", single_alarmed, ARRAY_SIZE(single_alarmed), 0);
	rc += test_select ("Single alarmed (ignored)", single_alarmed, ARRAY_SIZE(single_alarmed), 0);
	rc += test_select ("Single non-slave", single_non_slave, ARRAY_SIZE(single_non_slave), 0);
	rc += test_select ("Double", two, ARRAY_SIZE(two), 1);
	rc += test_select ("Double alarmed", two_alarmed, ARRAY_SIZE(two_alarmed), 0);
	rc += test_select ("Double all alarmed", two_all_alarmed, ARRAY_SIZE(two_all_alarmed), 1);
	rc += test_select ("Double stepped", two_stepped, ARRAY_SIZE(two_stepped), 0);
	rc += test_select ("Double slave and non-slave", two_slave_and_non_slave,
			   ARRAY_SIZE(two_slave_and_non_slave), 1);
	rc += test_select ("Double non-slave", two_non_slave_diff, ARRAY_SIZE(two_non_slave_diff), 1);
	rc += test_select ("Double non-slave", two_non_slave_same, ARRAY_SIZE(two_non_slave_same), 0);

	sfptpd_bic_select_instance (two, ARRAY_SIZE(two), &two [0]);
	rc += test_select ("Double (manual 0)", two, ARRAY_SIZE(two), 0);

	sfptpd_bic_select_instance (two, ARRAY_SIZE(two), &two [1]);
	rc += test_select ("Double (manual 1)", two, ARRAY_SIZE(two), 1);

	rc += test_select ("PTP and NTP", ptp_ntp, ARRAY_SIZE(ptp_ntp), 0);

	return rc == 0 ? 0 : EINVAL;
}


/* fin */
