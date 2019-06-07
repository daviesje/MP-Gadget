#include <math.h>
#include <string.h>

#include "allvars.h"
#include "timebinmgr.h"

#include "utils.h"
#include "uvbg.h"

/*! table with desired sync points. All forces and phase space variables are synchonized to the same order. */
static SyncPoint SyncPoints[8192];
static int NSyncPoints;    /* number of times stored in table of desired sync points */

/* This function compiles
 *
 * All.OutputListTimes, All.TimeIC, All.TimeMax
 *
 * into a list of SyncPoint objects.
 *
 * A SyncPoint is a time step where all state variables are at the same time on the
 * KkdkK timeline.
 *
 * TimeIC and TimeMax are used to ensure restarting from snapshot obtains exactly identical
 * integer stamps.
 **/
void
setup_sync_points(double TimeIC, double no_snapshot_until_time)
{
    int i;

    memset(&SyncPoints[0], -1, sizeof(SyncPoints[0]) * 8192);

    /* Set up first and last entry to SyncPoints; TODO we can insert many more! */

    // TODO(smutch): Add sync points for the UVBG calculation
    SyncPoints[0].a = TimeIC;
    SyncPoints[0].loga = log(TimeIC);
    SyncPoints[0].write_snapshot = 0; /* by default no output here. */
    SyncPoints[0].write_fof = 0;
    SyncPoints[0].calc_uvbg = 0;
    NSyncPoints = 1;

    // UVBG calculation every 10 Myr from z=20
    {
        double z_start = 20.;
        double a = 1.0 / (1.0 + z_start);

        while (a <= All.TimeMax) {
            SyncPoints[NSyncPoints].a = a;
            SyncPoints[NSyncPoints].loga = log(a);
            SyncPoints[NSyncPoints].write_snapshot = 1;
            SyncPoints[NSyncPoints].write_fof = 0;
            SyncPoints[NSyncPoints++].calc_uvbg = 1;

            // TODO(smutch): OK - this is ridiculous (sorry!), but I just wanted to quickly hack something...
            double delta_a = 0.0001;
            double lbt = time_to_present(a);
            double delta_lbt = 0.0;
            while ((delta_lbt <= 10.0) && (a <= All.TimeMax)) {
                a += delta_a;
                delta_lbt = lbt - time_to_present(a);
            }
        }
    }

    /* we do an insertion sort here. A heap is faster but who cares the speed for this? */
    for(i = 0; i < All.OutputListLength; i ++) {
        int j = 0;
        double a = All.OutputListTimes[i];
        double loga = log(a);

        for(j = 0; j < NSyncPoints; j ++) {
            if(a <= SyncPoints[j].a) {
                break;
            }
        }
        if(j == NSyncPoints) {
            /* beyond TimeMax, skip */
            continue;
        }
        /* found, so loga >= SyncPoints[j].loga */
        if(a == SyncPoints[j].a) {
            /* requesting output on an existing entry, e.g. TimeInit or duplicated entry */
        } else {
            /* insert the item; */
            memmove(&SyncPoints[j + 1], &SyncPoints[j],
                sizeof(SyncPoints[0]) * (NSyncPoints - j));
            SyncPoints[j].a = a;
            SyncPoints[j].loga = loga;
            NSyncPoints ++;
        }
        if(SyncPoints[j].a > no_snapshot_until_time) {
            SyncPoints[j].write_snapshot = 1;
            if(All.SnapshotWithFOF) {
                SyncPoints[j].write_fof = 1;
            }
        } else {
            SyncPoints[j].write_snapshot = 0;
            SyncPoints[j].write_fof = 0;
            SyncPoints[j].calc_uvbg = 0;
        }
    }

    for(i = 0; i < NSyncPoints; i++) {
        SyncPoints[i].ti = (i * 1L) << (TIMEBINS);
    }

/*     for(i = 0; i < NSyncPoints; i++) { */
/*         message(1,"Out: %g %ld\n", exp(SyncPoints[i].loga), SyncPoints[i].ti); */
/*     } */
}

/*! this function returns the next output time that is in the future of
 *  ti_curr; if none is find it return NULL, indication the run shall terminate.
 */
SyncPoint *
find_next_sync_point(inttime_t ti)
{
    int i;
    for(i = 0; i < NSyncPoints; i ++) {
        if(SyncPoints[i].ti > ti) {
            return &SyncPoints[i];
        }
    }
    return NULL;
}

/* This function finds if ti is a sync point; if so returns the sync point;
 * otherwise, NULL. We check if we shall write a snapshot with this. */
SyncPoint *
find_current_sync_point(inttime_t ti)
{
    int i;
    for(i = 0; i < NSyncPoints; i ++) {
        if(SyncPoints[i].ti == ti) {
            return &SyncPoints[i];
        }
    }
    return NULL;
}

/* Each integer time stores in the first 10 bits the snapshot number.
 * Then the rest of the bits are the standard integer timeline,
 * which should be a power-of-two hierarchy. We use this bit trick to speed up
 * the dloga look up. But the additional math makes this quite fragile. */

/*Gets Dloga / ti for the current integer timeline.
 * Valid up to the next snapshot, after which it will change*/
static double
Dloga_interval_ti(inttime_t ti)
{
    /* FIXME: This uses the bit tricks because it has to be fast
     * -- till we clean up the calls to loga_from_ti; then we can avoid bit tricks. */

    inttime_t lastsnap = ti >> TIMEBINS;

    if(lastsnap >= NSyncPoints - 1) {
        /* stop advancing loga after the last sync point. */
        return 0;
    }
    double lastoutput = SyncPoints[lastsnap].loga;
    return (SyncPoints[lastsnap+1].loga - lastoutput)/TIMEBASE;
}

double
loga_from_ti(inttime_t ti)
{
    inttime_t lastsnap = ti >> TIMEBINS;
    if(lastsnap > NSyncPoints) {
        endrun(1, "Requesting becond last sync point\n");
    }
    double last = SyncPoints[lastsnap].loga;
    inttime_t dti = ti & (TIMEBASE - 1);
    double logDTime = Dloga_interval_ti(ti);
    return last + dti * logDTime;
}

inttime_t
ti_from_loga(double loga)
{
    int i;
    int ti;
    for(i = 0; i < NSyncPoints - 1; i++)
    {
        if(SyncPoints[i].loga > loga)
            break;
    }
    /*If loop didn't trigger, i == All.NSyncPointTimes-1*/
    double logDTime = (SyncPoints[i].loga - SyncPoints[i-1].loga)/TIMEBASE;
    ti = (i-1) << TIMEBINS;
    /* Note this means if we overrun the end of the timeline,
     * we still get something reasonable*/
    ti += (loga - SyncPoints[i-1].loga)/logDTime;
    return ti;
}

double
dloga_from_dti(inttime_t dti)
{
    double Dloga = Dloga_interval_ti(All.Ti_Current);
    int sign = 1;
    if(dti < 0) {
        dti = -dti;
        sign = -1;
    }
    if((unsigned int) dti > TIMEBASE) {
        endrun(1, "Requesting dti larger than TIMEBASE\n");
    }
    return Dloga * dti * sign;
}

/* This function is only used for testing. Do not use in code. */
inttime_t
dti_from_dloga(double loga)
{
    int ti = ti_from_loga(loga_from_ti(All.Ti_Current));
    int tip = ti_from_loga(loga+loga_from_ti(All.Ti_Current));
    return tip - ti;
}

double
get_dloga_for_bin(int timebin)
{
    double logDTime = Dloga_interval_ti(All.Ti_Current);
    return (timebin > 0 ? (1u << (unsigned) timebin) : 0 ) * logDTime;
}

inttime_t
round_down_power_of_two(inttime_t dti)
{
    /* make dti a power 2 subdivision */
    inttime_t ti_min = TIMEBASE;
    while(ti_min > dti)
        ti_min >>= 1;
    return ti_min;
}

