#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mpi.h>
#include <gsl/gsl_sf_gamma.h>

#include "allvars.h"
#include "proto.h"
#include "cooling.h"
#include "forcetree.h"

#include "petaio.h"
#include "domain.h"
#include "mpsort.h"
#include "mymalloc.h"
#include "fof.h"

/*! \file init.c
 *  \brief code for initialisation of a simulation from initial conditions
 */

/*! This function reads the initial conditions, and allocates storage for the
 *  tree(s). Various variables of the particle data are initialised and An
 *  intial domain decomposition is performed. If SPH particles are present,
 *  the inial SPH smoothing lengths are determined.
 */
void init(void)
{
    int i, j;

#ifdef START_WITH_EXTRA_NGBDEV
    double MaxNumNgbDeviationMerk;
#endif

    set_global_time(All.TimeBegin);

    if(RestartFlag == 3 && RestartSnapNum < 0)
    {
        if(ThisTask == 0)
            endrun(0, "Need to give the snapshot number if FOF is selected for output\n");
    }

    if(RestartFlag == 4 && RestartSnapNum < 0)
    {
        if(ThisTask == 0)
            endrun(0, "Need to give the snapshot number if snapshot should be converted\n");
    }

    if(RestartFlag >= 2 && RestartSnapNum >= 0)  {
        petaio_read_snapshot(RestartSnapNum);
    } else
    if(RestartFlag == 0) {
        petaio_read_ic();
    } else {
        if(ThisTask == 0) {
            endrun(0, "RestartFlag and SnapNum combination is unknown");
        }
    }

    /* this ensures the initial BhP array is consistent */
    domain_garbage_collection_bh();

    set_global_time(All.TimeBegin);
#ifdef COOLING
    IonizeParams();
#endif

    All.Timebase_interval = (log(All.TimeMax) - log(All.TimeBegin)) / TIMEBASE;
    All.Ti_Current = 0;

    set_softenings();

    All.NumCurrentTiStep = 0;	/* setup some counters */
    All.SnapshotFileCount = 0;
    if(RestartFlag == 2)
    {
        if(RestartSnapNum < 0)
            All.SnapshotFileCount = atoi(All.InitCondFile + strlen(All.InitCondFile) - 3) + 1;
        else
            All.SnapshotFileCount = RestartSnapNum + 1;
    }

    All.TotNumOfForces = 0;
    All.NumForcesSinceLastDomainDecomp = 0;

    All.TreeAllocFactor = 0.7;


    All.Cadj_Cost = 1.0e-30;
    All.Cadj_Cpu = 1.0e-3;

    check_omega();

    All.TimeLastStatistics = All.TimeBegin - All.TimeBetStatistics;
#ifdef BLACK_HOLES
    All.TimeNextSeedingCheck = All.TimeBegin;
#endif

    for(i = 0; i < NumPart; i++)	/*  start-up initialization */
    {
        P[i].GravCost = 1;

#ifdef BLACK_HOLES
        if(RestartFlag == 0 && P[i].Type == 5 )
        {
            BHP(i).Mass = All.SeedBlackHoleMass;
        }
#endif
    }

    for(i = 0; i < TIMEBINS; i++)
        TimeBinActive[i] = 1;

    reconstruct_timebins();

    All.PM_Ti_endstep = All.PM_Ti_begstep = 0;

    for(i = 0; i < N_sph; i++)	/* initialize sph_properties */
    {
        for(j = 0; j < 3; j++)
        {
            SPHP(i).VelPred[j] = P[i].Vel[j];
            SPHP(i).HydroAccel[j] = 0;
        }

        SPHP(i).DtEntropy = 0;

        if(RestartFlag == 0)
        {
            SPHP(i).Density = -1;
#ifdef DENSITY_INDEPENDENT_SPH
            SPHP(i).EgyWtDensity = -1;
            SPHP(i).EntVarPred = -1;
#endif
#ifdef VOLUME_CORRECTION
            SPHP(i).DensityOld = 1;
#endif
#ifdef COOLING
            SPHP(i).Ne = 1.0;
#endif
            SPHP(i).DivVel = 0;
        }
#ifdef WINDS
        SPHP(i).DelayTime = 0;
#endif
#ifdef SFR
        SPHP(i).Sfr = 0;
#endif

#ifdef BLACK_HOLES
        SPHP(i).Injected_BH_Energy = 0;
#endif
#ifdef TWODIMS
        SPHP(i).VelPred[2] = 0;
        SPHP(i).HydroAccel[2] = 0;
#endif
#ifdef ONEDIM
        SPHP(i).VelPred[1] = SPHP(i).VelPred[2] = 0;
        SPHP(i).HydroAccel[1] =SPHP(i).HydroAccel[2] = 0;
#endif
    }


    test_id_uniqueness();

    Flag_FullStep = 1;		/* to ensure that Peano-Hilbert order is done */

    domain_Decomposition();	/* do initial domain decomposition (gives equal numbers of particles) */

    set_softenings();

    /* will build tree */
    ngb_treebuild();

    All.Ti_Current = 0;

#ifdef START_WITH_EXTRA_NGBDEV
    MaxNumNgbDeviationMerk = All.MaxNumNgbDeviation;
    All.MaxNumNgbDeviation = All.MaxNumNgbDeviationStart;
#endif

    if(RestartFlag != 3)
        setup_smoothinglengths();

#ifdef START_WITH_EXTRA_NGBDEV
    All.MaxNumNgbDeviation = MaxNumNgbDeviationMerk;
#endif
}


/*! This routine computes the mass content of the box and compares it to the
 * specified value of Omega-matter.  If discrepant, the run is terminated.
 */
void check_omega(void)
{
    double mass = 0, masstot, omega;
    int i;

    for(i = 0; i < NumPart; i++)
        mass += P[i].Mass;

    MPI_Allreduce(&mass, &masstot, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

    omega =
        masstot / (All.BoxSize * All.BoxSize * All.BoxSize) / (3 * All.Hubble * All.Hubble / (8 * M_PI * All.G));

    if(fabs(omega - All.Omega0) > 1.0e-3)
    {
        if(ThisTask == 0)
        {
            printf("\n\nI've found something odd!\n");
            printf
                ("The mass content accounts only for Omega=%g,\nbut you specified Omega=%g in the parameterfile.\n",
                 omega, All.Omega0);

            endrun(1, "I better stop");
        }
    }
}



/*! This function is used to find an initial smoothing length for each SPH
 *  particle. It guarantees that the number of neighbours will be between
 *  desired_ngb-MAXDEV and desired_ngb+MAXDEV. For simplicity, a first guess
 *  of the smoothing length is provided to the function density(), which will
 *  then iterate if needed to find the right smoothing length.
 */
void setup_smoothinglengths(void)
{
    int i;

    if(RestartFlag == 0)
    {
#pragma omp parallel for
        for(i = 0; i < NumPart; i++)
        {
            int no, p;
            no = Father[i];
            /* quick hack to adjust for the baryon fraction
             * only this fraction of mass is of that type.
             * this won't work for non-dm non baryon;
             * ideally each node shall have separate count of
             * ptypes of each type.
             *
             * Eventually the iteration will fix this. */
            double massfactor;
            if(P[i].Type == 0) {
                massfactor = 0.04 / 0.26;
            } else {
                massfactor = 1.0 - 0.04 / 0.26;
            }
            while(10 * All.DesNumNgb * P[i].Mass > massfactor * Nodes[no].u.d.mass)
            {
                p = Nodes[no].u.d.father;

                if(p < 0)
                    break;

                no = p;
            }

#ifndef TWODIMS
#ifndef ONEDIM
            P[i].Hsml =
                pow(3.0 / (4 * M_PI) * All.DesNumNgb * P[i].Mass / (massfactor * Nodes[no].u.d.mass),
                        1.0 / 3) * Nodes[no].len;
#else
            P[i].Hsml = All.DesNumNgb * (P[i].Mass / (massfactor * Nodes[no].u.d.mass)) * Nodes[no].len;
#endif
#else
            P[i].Hsml =
                pow(1.0 / (M_PI) * All.DesNumNgb * P[i].Mass / (massfactor * Nodes[no].u.d.mass), 1.0 / 2) * Nodes[no].len;
#endif
            if(All.SofteningTable[0] != 0 && P[i].Hsml > 500.0 * All.SofteningTable[0])
                P[i].Hsml = All.SofteningTable[0];
        }
    }

#ifdef BLACK_HOLES
    if(RestartFlag == 0 || RestartFlag == 2)
    {
        for(i = 0; i < NumPart; i++)
            if(P[i].Type == 5) {
                P[i].Hsml = All.SofteningTable[5];
                BHP(i).TimeBinLimit = -1;
            }
    }
#endif

    density();

    /* for clean IC with U input only, we need to iterate to find entrpoy */
    if(RestartFlag == 0)
    {
        const double a3 = All.Time * All.Time * All.Time;
#ifdef DENSITY_INDEPENDENT_SPH
        for(i = 0; i < N_sph; i++)
        {
            /* start the iteration from mass density */
            SPHP(i).EgyWtDensity = SPHP(i).Density;
        }


        /* initialization of the entropy variable is a little trickier in this version of SPH,
           since we need to make sure it 'talks to' the density appropriately */
        if (ThisTask == 0) {
            printf("Converting u -> entropy, with density split sph\n");
        }

        int j;
        double badness;
        double * olddensity = (double *)mymalloc("olddensity ", N_sph * sizeof(double));
        for(j=0;j<100;j++)
        {/* since ICs give energies, not entropies, need to iterate get this initialized correctly */
#pragma omp parallel for
            for(i = 0; i < N_sph; i++)
            {
                double entropy = GAMMA_MINUS1 * SPHP(i).Entropy / pow(SPHP(i).EgyWtDensity / a3 , GAMMA_MINUS1);
                SPHP(i).EntVarPred = pow(entropy, 1/GAMMA);
                olddensity[i] = SPHP(i).EgyWtDensity;
            }
            density();
            badness = 0;

#pragma omp parallel private(i)
            {
                double mybadness = 0;
#pragma omp for
                for(i = 0; i < N_sph; i++) {
                    if(!(SPHP(i).EgyWtDensity > 0)) continue;
                    double value = fabs(SPHP(i).EgyWtDensity - olddensity[i]) / SPHP(i).EgyWtDensity;
                    if(value > mybadness) mybadness = value;
                }
#pragma omp critical
                {
                    if(mybadness > badness) {
                        badness = mybadness;
                    }
                }
            }
            MPI_Allreduce(MPI_IN_PLACE, &badness, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

            if(ThisTask == 0)
                printf("iteration %03d, max relative difference = %g \n", j, badness);

            if(badness < 1e-3) break;
        }
        myfree(olddensity);
#endif //DENSITY_INDEPENDENT_SPH
#pragma omp parallel for
        for(i = 0; i < N_sph; i++) {
            /* EgyWtDensity stabilized, now we convert from energy to entropy*/
            SPHP(i).Entropy = GAMMA_MINUS1 * SPHP(i).Entropy / pow(SPHP(i).EOMDensity/a3 , GAMMA_MINUS1);
        }
    }

#ifdef DENSITY_INDEPENDENT_SPH
    /* snapshot already has Entropy and EgyWtDensity;
     * hope it is read in correctly. (need a test
     * on this!) */
    /* regardless we initalize EntVarPred. This may be unnecessary*/
    for(i = 0; i < N_sph; i++) {
        SPHP(i).EntVarPred = pow(SPHP(i).Entropy, 1./GAMMA);
    }
    density();
#endif //DENSITY_INDEPENDENT_SPH
}
