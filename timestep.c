#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "allvars.h"
#include "proto.h"
#include "timefac.h"
#include "cosmology.h"
#include "cooling.h"
#include "mymalloc.h"
#include "endrun.h"
#include "system.h"
#include "timestep.h"

/*! \file timestep.c
 *  \brief routines for 'kicking' particles in
 *  momentum space and assigning new timesteps
 */


/*Flat array containing all active particles*/
int NumActiveParticle;
int *ActiveParticle;

int TimeBinCount[TIMEBINS];
int TimeBinCountSph[TIMEBINS];
int TimeBinActive[TIMEBINS];

void timestep_allocate_memory(int MaxPart)
{
    ActiveParticle = (int *) mymalloc("ActiveParticle", MaxPart * sizeof(int));
}

static void reverse_and_apply_gravity();
static int get_timestep(int p, int dt_max);
static int get_timestep_bin(int ti_step);
static void do_the_kick(int i, int tstart, int tend, int tcurrent, double dt_gravkick);
static void advance_long_range_kick(int PM_Timestep);

int is_timebin_active(int i) {
    return TimeBinActive[i];
}

/*! This function sets the (comoving) softening length of all particle
 *  types in the table All.SofteningTable[...].  We check that the physical
 *  softening length is bounded by the Softening-MaxPhys values.
 */
void set_softenings(const double time)
{
    int i;

    if(All.SofteningGas * time > All.SofteningGasMaxPhys)
        All.SofteningTable[0] = All.SofteningGasMaxPhys / time;
    else
        All.SofteningTable[0] = All.SofteningGas;

    if(All.SofteningHalo * time > All.SofteningHaloMaxPhys)
        All.SofteningTable[1] = All.SofteningHaloMaxPhys / time;
    else
        All.SofteningTable[1] = All.SofteningHalo;

    if(All.SofteningDisk * time > All.SofteningDiskMaxPhys)
        All.SofteningTable[2] = All.SofteningDiskMaxPhys / time;
    else
        All.SofteningTable[2] = All.SofteningDisk;

    if(All.SofteningBulge * time > All.SofteningBulgeMaxPhys)
        All.SofteningTable[3] = All.SofteningBulgeMaxPhys / time;
    else
        All.SofteningTable[3] = All.SofteningBulge;

    if(All.SofteningStars * time > All.SofteningStarsMaxPhys)
        All.SofteningTable[4] = All.SofteningStarsMaxPhys / time;
    else
        All.SofteningTable[4] = All.SofteningStars;

    if(All.SofteningBndry * time > All.SofteningBndryMaxPhys)
        All.SofteningTable[5] = All.SofteningBndryMaxPhys / time;
    else
        All.SofteningTable[5] = All.SofteningBndry;

    for(i = 0; i < 6; i++)
        All.ForceSoftening[i] = 2.8 * All.SofteningTable[i];

    All.MinGasHsml = All.MinGasHsmlFractional * All.ForceSoftening[0];
}

void set_global_time(double newtime) {
    All.Time = newtime;

    All.cf.a = All.Time;
    All.cf.a2inv = 1 / (All.Time * All.Time);
    All.cf.a3inv = 1 / (All.Time * All.Time * All.Time);
    All.cf.fac_egy = pow(All.Time, 3 * GAMMA_MINUS1);
    All.cf.hubble = hubble_function(All.Time);
    All.cf.hubble_a2 = All.Time * All.Time * hubble_function(All.Time);

#ifdef LIGHTCONE
    lightcone_set_time(All.cf.a);
#endif
    IonizeParams();
    set_softenings(newtime);
}

/*! This function advances the system in momentum space, i. it does apply the 'kick' operation after the
 *  forces have been computed. Additionally, it assigns new timesteps to particles. At start-up, a
 *  half-timestep is carried out, as well as at the end of the simulation. In between, the half-step kick that
 *  ends the previous timestep and the half-step kick for the new timestep are combined into one operation.
 */
void advance_and_find_timesteps(void)
{
    int pa;

    walltime_measure("/Misc");

    const double dt_gravkickB = get_gravkick_factor(All.PM_Ti_begstep, All.Ti_Current) -
            get_gravkick_factor(All.PM_Ti_begstep, (All.PM_Ti_begstep + All.PM_Ti_endstep) / 2);

    if(All.MakeGlassFile)
        reverse_and_apply_gravity();

    /* Now assign new timesteps and kick */
#ifdef FORCE_EQUAL_TIMESTEPS
    int ti_min=TIMEBASE;
    #pragma omp parallel for
    for(pa = 0; pa < NumActiveParticle; pa++)
    {
        const int i = ActiveParticle[pa];
        int ti_step = get_timestep(i,All.MaxTimeStepDisplacement);

        if(ti_step < ti_min)
            ti_min = ti_step;
    }

    int ti_min_glob;

    MPI_Allreduce(&ti_min, &ti_min_glob, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
#endif

    int badstepsizecount = 0;
    #pragma omp parallel for
    for(pa = 0; pa < NumActiveParticle; pa++)
    {
        const int i = ActiveParticle[pa];
#ifdef FORCE_EQUAL_TIMESTEPS
        int ti_step = ti_min_glob;
#else
        int ti_step = get_timestep(i,All.MaxTimeStepDisplacement);
#endif
        /* make it a power 2 subdivision */
        int ti_min = TIMEBASE;
        while(ti_min > ti_step)
            ti_min >>= 1;
        ti_step = ti_min;

        int bin = get_timestep_bin(ti_step);
        if(bin < 1) {
            message(1, "Time-step of integer size %d not allowed, id = %lu, debugging info follows. %d\n", ti_step, P[i].ID);
            badstepsizecount++;
        }
        int binold = P[i].TimeBin;

        if(bin > binold)		/* timestep wants to increase */
        {
            /* make sure the new step is currently active,
             * so that particles do not miss a step */
            while(TimeBinActive[bin] == 0 && bin > binold)
                bin--;

            ti_step = bin ? (1 << bin) : 0;
        }

        if(All.Ti_Current >= TIMEBASE)	/* we here finish the last timestep. */
        {
            ti_step = 0;
            bin = 0;
        }

        if((TIMEBASE - All.Ti_Current) < ti_step)	/* check that we don't run beyond the end */
        {
            endrun(888, "Integer timeline ran past the end of the bins: %d - %d  < %d\n",TIMEBASE, All.Ti_Current, ti_step);
        }

        /* This moves particles between time bins:
         * active particles always remain active
         * until reconstruct_timebins is called
         * (during domain, on new timestep).*/
        if(bin != binold)
        {
            /*Update time bin counts*/
            atomic_fetch_and_add(&TimeBinCount[binold],-1);
            atomic_fetch_and_add(&TimeBinCount[bin],1);
            if(P[i].Type == 0)
            {
                atomic_fetch_and_add(&TimeBinCountSph[binold],-1);
                atomic_fetch_and_add(&TimeBinCountSph[bin],1);
            }
            P[i].TimeBin = bin;
        }

        int ti_step_old = binold ? (1 << binold) : 0;

        int tstart = P[i].Ti_begstep + ti_step_old / 2;	/* midpoint of old step */
        int tend = P[i].Ti_begstep + ti_step_old + ti_step / 2;	/* midpoint of new step */

        P[i].Ti_begstep += ti_step_old;

        /*This only changes particle i, so is thread-safe.*/
        do_the_kick(i, tstart, tend, P[i].Ti_begstep,dt_gravkickB);
    }

    /*Check whether any particles had a bad timestep*/
    int badstepsizecount_global=0;
    MPI_Allreduce(&badstepsizecount, &badstepsizecount_global, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    if(badstepsizecount_global) {
        message(0, "bad timestep spotted: terminating and saving snapshot.\n");
        All.NumCurrentTiStep = 0;
        savepositions(999999, 0);
        endrun(0, "Ending due to bad timestep");
    }


    if(All.PM_Ti_endstep == All.Ti_Current)	/* need to do long-range kick */
    {
        advance_long_range_kick(All.MaxTimeStepDisplacement);
    }

    walltime_measure("/Timeline");
}

/*Advance a long-range timestep and do the desired kick.*/
void advance_long_range_kick(int PM_Timestep)
{
    int i;
    int ti_step = TIMEBASE;
    while(ti_step > PM_Timestep)
        ti_step >>= 1;
    /*Make it a little larger so it will go through the output time.*/
    ti_step <<=1;

    if(All.Ti_Current == TIMEBASE)	/* we here finish the last timestep. */
        ti_step = 0;

    const int tstart = (All.PM_Ti_begstep + All.PM_Ti_endstep) / 2;
    const int tend = All.PM_Ti_endstep + ti_step / 2;

    const double dt_gravkick = get_gravkick_factor(tstart, tend);

    All.PM_Ti_begstep = All.PM_Ti_endstep;
    All.PM_Ti_endstep = All.PM_Ti_begstep + ti_step;

    const double dt_gravkickB = -get_gravkick_factor(All.PM_Ti_begstep, (All.PM_Ti_begstep + All.PM_Ti_endstep) / 2);

    #pragma omp parallel for
    for(i = 0; i < NumPart; i++)
    {
        int j;
        for(j = 0; j < 3; j++)	/* do the kick */
            P[i].Vel[j] += P[i].GravPM[j] * dt_gravkick;

        if(P[i].Type == 0)
        {
            const int dt_step = (P[i].TimeBin ? (1 << P[i].TimeBin) : 0);

            const double dt_gravkickA = get_gravkick_factor(P[i].Ti_begstep, All.Ti_Current) -
                get_gravkick_factor(P[i].Ti_begstep, P[i].Ti_begstep + dt_step / 2);
            const double dt_hydrokick = get_hydrokick_factor(P[i].Ti_begstep, All.Ti_Current) -
                get_hydrokick_factor(P[i].Ti_begstep, P[i].Ti_begstep + dt_step / 2);

            for(j = 0; j < 3; j++)
                SPHP(i).VelPred[j] = P[i].Vel[j]
                    + P[i].GravAccel[j] * dt_gravkickA
                    + SPHP(i).HydroAccel[j] * dt_hydrokick + P[i].GravPM[j] * dt_gravkickB;
        }
    }
}

void do_the_kick(int i, int tstart, int tend, int tcurrent, double dt_gravkickB)
{
    double dt_entr = (tend - tstart) * All.Timebase_interval;
    const double dt_gravkick = get_gravkick_factor(tstart, tend);
    const double dt_hydrokick = get_hydrokick_factor(tstart, tend);
    const double dt_gravkick2 = get_gravkick_factor(tcurrent, tend);
    const double dt_hydrokick2 = get_hydrokick_factor(tcurrent, tend);
    int j;

    /* do the kick */

    for(j = 0; j < 3; j++)
    {
        P[i].Vel[j] += P[i].GravAccel[j] * dt_gravkick;
    }

    if(P[i].Type != 0)
        return;

    /* Add kick from hydro and SPH stuff */
    for(j = 0; j < 3; j++)
    {
        P[i].Vel[j] += SPHP(i).HydroAccel[j] * dt_hydrokick;

        SPHP(i).VelPred[j] =
            P[i].Vel[j] - dt_gravkick2 * P[i].GravAccel[j] - dt_hydrokick2 * SPHP(i).HydroAccel[j];

        SPHP(i).VelPred[j] += P[i].GravPM[j] * dt_gravkickB;
    }

    /* Code here imposes a hard limit (default to speed of light)
     * on the gas velocity. Then a limit on the change in entropy
     * FIXME: This should probably not be needed!*/
    const double velfac = sqrt(All.cf.a3inv);
    double vv=0;
    for(j=0; j < 3; j++)
        vv += P[i].Vel[j] * P[i].Vel[j];
    vv = sqrt(vv);
    if(vv > All.MaxGasVel * velfac)
        for(j=0;j < 3; j++)
        {
            P[i].Vel[j] *= All.MaxGasVel * velfac / vv;
            SPHP(i).VelPred[j] =
                P[i].Vel[j] - dt_gravkick2 * P[i].GravAccel[j] - dt_hydrokick2 * SPHP(i).HydroAccel[j];

            SPHP(i).VelPred[j] += P[i].GravPM[j] * dt_gravkickB;
        }

    /* In case of cooling, we prevent that the entropy (and
       hence temperature) decreases by more than a factor 0.5.
       FIXME: Why is this and the last thing here? Should not be needed. */

    if(SPHP(i).DtEntropy * dt_entr > -0.5 * SPHP(i).Entropy)
        SPHP(i).Entropy += SPHP(i).DtEntropy * dt_entr;
    else
        SPHP(i).Entropy *= 0.5;

    /* Implement an entropy floor*/
    if(All.MinEgySpec)
    {
        const double minentropy = All.MinEgySpec * GAMMA_MINUS1 / pow(SPHP(i).EOMDensity * All.cf.a3inv, GAMMA_MINUS1);
        if(SPHP(i).Entropy < minentropy)
        {
            SPHP(i).Entropy = minentropy;
            SPHP(i).DtEntropy = 0;
        }
    }

    /* In case the timestep increases in the new step, we
       make sure that we do not 'overcool'. */
    dt_entr = get_dtime(P[i].TimeBin) / 2;

    if(SPHP(i).Entropy + SPHP(i).DtEntropy * dt_entr < 0.5 * SPHP(i).Entropy)
        SPHP(i).DtEntropy = -0.5 * SPHP(i).Entropy / dt_entr;

}



/*! This function normally (for flag==0) returns the maximum allowed timestep of a particle, expressed in
 *  terms of the integer mapping that is used to represent the total simulated timespan.
 *  Arguments:
 *  p -> particle index
 *  dt_max -> maximal timestep.  */
int get_timestep(const int p, const int dt_max)
{
    double ac = 0;
    double dt = 0, dt_courant = 0;
    int ti_step;
    /*Give a useful message if we are broken*/
    if(dt_max == 0)
        return 0;
    /*Set to max timestep allowed if the tree is off*/
    if(!All.TreeGravOn)
        return dt_max;

    /*Compute physical acceleration*/
    {
        double ax = All.cf.a2inv * P[p].GravAccel[0];
        double ay = All.cf.a2inv * P[p].GravAccel[1];
        double az = All.cf.a2inv * P[p].GravAccel[2];

        ax += All.cf.a2inv * P[p].GravPM[0];
        ay += All.cf.a2inv * P[p].GravPM[1];
        az += All.cf.a2inv * P[p].GravPM[2];

        if(P[p].Type == 0)
        {
            const double fac2 = 1 / pow(All.Time, 3 * GAMMA - 2);
            ax += fac2 * SPHP(p).HydroAccel[0];
            ay += fac2 * SPHP(p).HydroAccel[1];
            az += fac2 * SPHP(p).HydroAccel[2];
        }

        ac = sqrt(ax * ax + ay * ay + az * az);	/* this is now the physical acceleration */
    }

    if(ac == 0)
        ac = 1.0e-30;

    dt = sqrt(2 * All.ErrTolIntAccuracy * All.cf.a * All.SofteningTable[P[p].Type] / ac);
#ifdef ADAPTIVE_GRAVSOFT_FORGAS
    if(P[p].Type == 0)
        dt = sqrt(2 * All.ErrTolIntAccuracy * All.cf.a * P[p].Hsml / 2.8 / ac);
#endif

    if(P[p].Type == 0)
    {
        const double fac3 = pow(All.Time, 3 * (1 - GAMMA) / 2.0);
        dt_courant = 2 * All.CourantFac * All.Time * P[p].Hsml / (fac3 * SPHP(p).MaxSignalVel);
        if(dt_courant < dt)
            dt = dt_courant;
    }

#ifdef BLACK_HOLES
    if(P[p].Type == 5)
    {
        if(BHP(p).Mdot > 0 && BHP(p).Mass > 0)
        {
            double dt_accr = 0.25 * BHP(p).Mass / BHP(p).Mdot;
            if(dt_accr < dt)
                dt = dt_accr;
        }
        if(BHP(p).TimeBinLimit > 0) {
            double dt_limiter = get_dtime(BHP(p).TimeBinLimit)/All.cf.hubble;
            if (dt_limiter < dt) dt = dt_limiter;
        }
    }
#endif

    /* convert the physical timestep to dloga if needed. Note: If comoving integration has not been selected,
       All.cf.hubble=1.
       */
    dt *= All.cf.hubble;

    if(dt < All.MinSizeTimestep)
        dt = All.MinSizeTimestep;

    ti_step = (int) (dt / All.Timebase_interval);

    if(ti_step > dt_max)
        ti_step = dt_max;


    if(!(ti_step > 1 && ti_step < TIMEBASE))
    {
        message(1, "Error: A timestep of size zero was assigned on the integer timeline!\n"
                "We better stop.\n"
                "Task=%d type %d Part-ID=%lu dt=%g dtc=%g dtdis=%g tibase=%g ti_step=%d ac=%g xyz=(%g|%g|%g) tree=(%g|%g|%g), dt0=%g, ErrTolIntAccuracy=%g\n\n",
                ThisTask, P[p].Type, (MyIDType)P[p].ID, dt, dt_courant, dt_max,
                All.Timebase_interval, ti_step, ac,
                P[p].Pos[0], P[p].Pos[1], P[p].Pos[2], P[p].GravAccel[0], P[p].GravAccel[1],
                P[p].GravAccel[2],
                sqrt(2 * All.ErrTolIntAccuracy * All.cf.a * All.SofteningTable[P[p].Type] / ac) * All.cf.hubble, All.ErrTolIntAccuracy
              );

        message(1, "pm_force=(%g|%g|%g)\n", P[p].GravPM[0], P[p].GravPM[1], P[p].GravPM[2]);

        if(P[p].Type == 0)
            message(1, "hydro-frc=(%g|%g|%g) dens=%g hsml=%g numngb=%g\n", SPHP(p).HydroAccel[0], SPHP(p).HydroAccel[1],
                    SPHP(p).HydroAccel[2], SPHP(p).Density, P[p].Hsml, P[p].NumNgb);
#ifdef DENSITY_INDEPENDENT_SPH
        if(P[p].Type == 0)
            message(1, "egyrho=%g entvarpred=%g dhsmlegydensityfactor=%g Entropy=%g, dtEntropy=%g, Pressure=%g\n", SPHP(p).EgyWtDensity, SPHP(p).EntVarPred,
                    SPHP(p).DhsmlEgyDensityFactor, SPHP(p).Entropy, SPHP(p).DtEntropy, SPHP(p).Pressure);
#endif
#ifdef SFR
        if(P[p].Type == 0) {
            message(1, "sfr = %g\n" , SPHP(p).Sfr);
        }
#endif
#ifdef BLACK_HOLES
        if(P[p].Type == 0) {
            message(1, "injected_energy = %g\n" , SPHP(p).Injected_BH_Energy);
        }
#endif
    }

    return ti_step;
}


/*! This function computes the PM timestep of the system based on
 *  the rms velocities of particles. For cosmological simulations, the criterion used is that the rms
 *  displacement should be at most a fraction MaxRMSDisplacementFac of the mean particle separation. 
 *  Note that the latter is estimated using the assigned particle masses, separately for each particle type.
 */
int find_dt_displacement_constraint()
{
    int i, type;
    int ti_step;
    int count[6];
    int64_t count_sum[6];
    double v[6], v_sum[6], mim[6], min_mass[6];
    double dt_disp = All.MaxSizeTimestep;

    for(type = 0; type < 6; type++)
    {
        count[type] = 0;
        v[type] = 0;
        mim[type] = 1.0e30;
    }

    for(i = 0; i < NumPart; i++)
    {
        v[P[i].Type] += P[i].Vel[0] * P[i].Vel[0] + P[i].Vel[1] * P[i].Vel[1] + P[i].Vel[2] * P[i].Vel[2];
        if(P[i].Mass > 0)
        {
            if(mim[P[i].Type] > P[i].Mass)
                mim[P[i].Type] = P[i].Mass;
        }
        count[P[i].Type]++;
    }

    MPI_Allreduce(v, v_sum, 6, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(mim, min_mass, 6, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);

    sumup_large_ints(6, count, count_sum);

#ifdef SFR
    /* add star and gas particles together to treat them on equal footing, using the original gas particle
       spacing. */
    v_sum[0] += v_sum[4];
    count_sum[0] += count_sum[4];
    v_sum[4] = v_sum[0];
    count_sum[4] = count_sum[0];
#ifdef BLACK_HOLES
    v_sum[0] += v_sum[5];
    count_sum[0] += count_sum[5];
    v_sum[5] = v_sum[0];
    count_sum[5] = count_sum[0];
    min_mass[5] = min_mass[0];
#endif
#endif

    for(type = 0; type < 6; type++)
    {
        if(count_sum[type] > 0)
        {
            double omega, dmean, dt;
            const double asmth = All.Asmth * All.BoxSize / All.Nmesh;
            if(type == 0 || (type == 4 && All.StarformationOn)
#ifdef BLACK_HOLES
                || (type == 5)
#endif
                ) {
                omega = All.CP.OmegaBaryon;
            }
            /* Neutrinos are counted here as CDM. They should be counted separately!
             * In practice usually FastParticleType == 2
             * so this doesn't matter. Also the neutrinos
             * are either Way Too Fast, or basically CDM anyway. */
            else {
                omega = All.CP.OmegaCDM;
            }
            /* "Avg. radius" of smallest particle: (min_mass/total_mass)^1/3 */
            dmean = pow(min_mass[type] / (omega * 3 * All.Hubble * All.Hubble / (8 * M_PI * All.G)), 1.0 / 3);

            dt = All.MaxRMSDisplacementFac * All.cf.hubble * All.cf.a * All.cf.a * DMIN(asmth, dmean) / sqrt(v_sum[type] / count_sum[type]);
            message(0, "type=%d  dmean=%g asmth=%g minmass=%g a=%g  sqrt(<p^2>)=%g  dlogmax=%g\n",
                    type, dmean, asmth, min_mass[type], All.Time, sqrt(v_sum[type] / count_sum[type]), dt);

            /* don't constrain the step to the neutrinos */
            if(type != All.FastParticleType && dt < dt_disp)
                dt_disp = dt;
        }
    }

    ti_step = dt_disp / All.Timebase_interval;
    /* Make sure that we finish the PM step before the next output.
     * This is important for best restart accuracy: it ensures that
     * when GravPM and GravAccel are reset to zero, their effect
     * has already been included.*/
    if(All.Ti_nextoutput > All.PM_Ti_endstep) {
        /*If the next PM step finishes after or just before the next snapshot output, extend it a little*/
        if(1.1*ti_step + All.PM_Ti_endstep > All.Ti_nextoutput) {
            ti_step = All.Ti_nextoutput - All.PM_Ti_endstep;
        }
    }
    message(0, "Maximal PM timestep: %g  (%g)\n", ti_step*All.Timebase_interval, All.MaxSizeTimestep);
    return ti_step;
}

int get_timestep_bin(int ti_step)
{
   int bin = -1;

   if(ti_step == 0)
       return 0;

   if(ti_step == 1)
   {
       return -1;
   }

   while(ti_step)
   {
       bin++;
       ti_step >>= 1;
   }

   return bin;
}

/* This function reverse the direction of the gravitational force.
 * This is only useful for making Lagrangian glass files*/
void reverse_and_apply_gravity()
{
    double dispmax=0, globmax;
    int i;
    for(i = 0; i < NumPart; i++)
    {
        int j;
        /*Reverse the direction of acceleration*/
        for(j = 0; j < 3; j++)
        {
            P[i].GravAccel[j] *= -1;
            P[i].GravAccel[j] -= P[i].GravPM[j];
            P[i].GravPM[j] = 0;
        }

        double disp = sqrt(P[i].GravAccel[0] * P[i].GravAccel[0] +
                P[i].GravAccel[1] * P[i].GravAccel[1] + P[i].GravAccel[2] * P[i].GravAccel[2]);

        disp *= 2.0 / (3 * All.Hubble * All.Hubble);

        if(disp > dispmax)
            dispmax = disp;
    }

    MPI_Allreduce(&dispmax, &globmax, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

    double dmean = pow(P[0].Mass / (All.CP.Omega0 * 3 * All.Hubble * All.Hubble / (8 * M_PI * All.G)), 1.0 / 3);

    const double fac = DMIN(1.0, dmean / globmax);

    message(0, "Glass-making: dmean= %g  global disp-maximum= %g\n", dmean, globmax);

    /* Move the actual particles according to the (reversed) gravitational force.
     * Not sure why this is here rather than in the main code.*/
    for(i = 0; i < NumPart; i++)
    {
        int j;
        for(j = 0; j < 3; j++)
        {
            P[i].Vel[j] = 0;
            P[i].Pos[j] += fac * P[i].GravAccel[j] * 2.0 / (3 * All.Hubble * All.Hubble);
            P[i].GravAccel[j] = 0;
        }
    }

}

void rebuild_activelist(void)
{
    int i;

    for(i = 0; i < TIMEBINS; i++)
    {
        TimeBinCount[i] = 0;
        TimeBinCountSph[i] = 0;
    }

    NumActiveParticle = 0;

    for(i = 0; i < NumPart; i++)
    {
        int bin = P[i].TimeBin;

        if(TimeBinActive[bin])
        {
            ActiveParticle[NumActiveParticle] = i;
            NumActiveParticle++;
        }
        TimeBinCount[bin]++;
        if(P[i].Type == 0)
            TimeBinCountSph[bin]++;
    }
}


/* mark the bins that will be active before the next kick*/
int find_active_timebins(int next_kick)
{
    int n;
    int NumForceUpdate = TimeBinCount[0];

    for(n = 1, TimeBinActive[0] = 1; n < TIMEBINS; n++)
    {
        int dt_bin = (1 << n);

        if((next_kick % dt_bin) == 0)
        {
            TimeBinActive[n] = 1;
            NumForceUpdate += TimeBinCount[n];
        }
        else
            TimeBinActive[n] = 0;
    }
    return NumForceUpdate;
}
