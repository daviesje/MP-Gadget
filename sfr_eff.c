/***
 * Multi-Phase star formaiton
 *
 * The algorithm here is based on Springel Hernequist 2003, and Okamoto 2010.
 *
 * The source code originally came from sfr_eff.c in Gadget-3. This version has
 * been heavily rewritten to add support for new wind models, new star formation
 * criterions, and more importantly, use use the new tree walker routines.
 *
 * I (Yu Feng) feel it is appropriate to release most of this file with a free license,
 * because the implementation here has diverged from the original code by too far.
 *
 * The largest remaining concern are a few functions there were obtained from Gadget-P. 
 * They are for * self-gravity starformation condition and H2.
 * Eventhough they have been heavily rewritten, the core math is the same.
 * the license is still murky. Do not use them unless Phil Hopkins has agreed.
 *
 * */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "allvars.h"
#include "proto.h"
#include "forcetree.h"
#include "cooling.h"
#include "domain.h"
#include "mymalloc.h"

#ifdef METALS
#define METALLICITY(i) (P[(i)].Metallicity)
#else
#define METALLICITY(i) (0.)
#endif

#ifdef COOLING
static double u_to_temp_fac; /* assuming very hot !*/

/* these guys really shall be local to cooling_and_starformation, but
 * I am too lazy to pass them around to subroutines.
 */
static int stars_converted;
static int stars_spawned;
static double sum_sm;
static double sum_mass_stars;

static void cooling_relaxed(int i, double egyeff, double dtime, double trelax);
static void cooling_direct(int i);
#ifdef WINDS
static int make_particle_wind(int i, double v, double vmean[3]);
#endif
#ifdef SFR
static int get_sfr_condition(int i);
static int make_particle_star(int i);
static void starformation(int i);
static double get_sfr_factor_due_to_selfgravity(int i);
static double get_sfr_factor_due_to_h2(int i);
static double get_starformation_rate_full(int i, double dtime, double * ne_new, double * trelax, double * egyeff);
#endif


#ifdef WINDS
struct winddata_in {
    int NodeList[NODELISTLENGTH];
    double Sfr;
    double Dt;
    double Pos[3];
    double Mass;
    double Hsml;
    double TotalWeight;
    double DMRadius;
    double Vdisp;
    double Vmean[3];
    MyIDType ID;
};

struct winddata_out {
    double TotalWeight;
    double V1sum[3];
    double V2sum;
    int Ngb;
};

static struct winddata {
    double DMRadius;
    double Left;
    double Right;
    double TotalWeight;
    union {
        double Vdisp;
        double V2sum;
    };
    union {
        double Vmean[3];
        double V1sum[3];
    };
    int Ngb;
} * Wind;


static int sfr_wind_isactive(int target);
static void sfr_wind_reduce_weight(int place, struct winddata_out * remote, int mode);
static void sfr_wind_copy(int place, struct winddata_in * input);
static int sfr_wind_ev_weight(int target, int mode,
        struct winddata_in * I,
        struct winddata_out * O,
        LocalEvaluator * lv);
static int sfr_wind_evaluate(int target, int mode,
        struct winddata_in * I,
        struct winddata_out * O,
        LocalEvaluator * lv);

#endif
/*
 * This routine does cooling and star formation for
 * the effective multi-phase model.
 */


static int sfr_cooling_isactive(int target) {
    return P[target].Type == 0;
}

void cooling_and_starformation(void)
    /* cooling routine when star formation is enabled */
{
    u_to_temp_fac = (4 / (8 - 5 * (1 - HYDROGEN_MASSFRAC))) * PROTONMASS / BOLTZMANN * GAMMA_MINUS1
        * All.UnitEnergy_in_cgs / All.UnitMass_in_g;

    walltime_measure("/Misc");

    stars_spawned = stars_converted = 0;
    sum_sm = sum_mass_stars = 0;

    Evaluator ev = {0};

    /* Only used to list all active particles for the parallel loop */
    /* no tree walking and no need to export / copy particles. */
    ev.ev_label = "SFR_COOL";
    ev.ev_isactive = sfr_cooling_isactive;

    int Nactive = 0;
    int * queue = ev_get_queue(&ev, &Nactive);
    int n;

#pragma omp parallel for
    for(n = 0; n < Nactive; n ++)
    {
        int i = queue[n];
        int flag;
#ifdef WINDS
        if(SPHP(i).DelayTime > 0) {
            double dt = (P[i].TimeBin ? (1 << P[i].TimeBin) : 0) * All.Timebase_interval;
                /*  the actual time-step */

            double dtime;

            dtime = dt / All.cf.hubble;

            SPHP(i).DelayTime -= dtime;
        }

        if(SPHP(i).DelayTime > 0) {
            if(SPHP(i).Density * All.cf.a3inv < All.WindFreeTravelDensFac * All.PhysDensThresh)
                SPHP(i).DelayTime = 0;
        } else {
            SPHP(i).DelayTime = 0;
        }
#endif

        /* check whether conditions for star formation are fulfilled.
         *
         * f=1  normal cooling
         * f=0  star formation
         */
        flag = get_sfr_condition(i);

        /* normal implicit isochoric cooling */
        if(flag == 1 || All.QuickLymanAlphaProbability > 0) {
            cooling_direct(i);
        }
#ifdef ENDLESSSTARS
        flag = 0;
#endif
        if(flag == 0) {
            /* active star formation */
            starformation(i);
        }
    } /*end of main loop over active particles */

    myfree(queue);

    int tot_spawned, tot_converted;
    MPI_Allreduce(&stars_spawned, &tot_spawned, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(&stars_converted, &tot_converted, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    if(tot_spawned > 0 || tot_converted > 0)
    {
        if(ThisTask == 0)
        {
            printf("SFR: spawned %d stars, converted %d gas particles into stars\n",
                    tot_spawned, tot_converted);
            fflush(stdout);
        }

//        All.TotN_sph -= tot_converted;

        /* Note: N_sph is only reduced once rearrange_particle_sequence is called */

        /* Note: New tree construction can be avoided because of  `force_add_star_to_tree()' */
    }

    double totsfrrate, localsfr=0;
    int i;
    #pragma omp parallel for reduction(+: localsfr)
    for(i = 0; i < N_sph; i++)
        localsfr += SPHP(i).Sfr;

    MPI_Allreduce(&localsfr, &totsfrrate, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

    double total_sum_mass_stars, total_sm;

    MPI_Reduce(&sum_sm, &total_sm, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&sum_mass_stars, &total_sum_mass_stars, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    if(ThisTask == 0)
    {
        double rate;
        double rate_in_msunperyear;
        if(All.TimeStep > 0)
            rate = total_sm / (All.TimeStep / (All.Time * All.cf.hubble));
        else
            rate = 0;

        /* convert to solar masses per yr */

        rate_in_msunperyear = rate * (All.UnitMass_in_g / SOLAR_MASS) / (All.UnitTime_in_s / SEC_PER_YEAR);

        fprintf(FdSfr, "%g %g %g %g %g\n", All.Time, total_sm, totsfrrate, rate_in_msunperyear,
                total_sum_mass_stars);
        fflush(FdSfr);
    }
    walltime_measure("/Cooling/StarFormation");
#ifdef WINDS
    /* now lets make winds. this has to be after NumPart is updated */
    if(!HAS(All.WindModel, WINDS_SUBGRID) && All.WindModel != WINDS_NONE) {
        int i;
        Wind = (struct winddata * ) mymalloc("WindExtraData", NumPart * sizeof(struct winddata));
        Evaluator ev = {0};

        ev.ev_label = "SFR_WIND";
        ev.ev_isactive = sfr_wind_isactive;
        ev.ev_copy = (ev_copy_func) sfr_wind_copy;
        ev.ev_reduce = (ev_reduce_func) sfr_wind_reduce_weight;
        ev.UseNodeList = 1;
        ev.ev_datain_elsize = sizeof(struct winddata_in);
        ev.ev_dataout_elsize = sizeof(struct winddata_out);

        /* sum the total weight of surrounding gas */
        ev.ev_evaluate = (ev_ev_func) sfr_wind_ev_weight;
        int Nqueue;
        int * queue = ev_get_queue(&ev, &Nqueue);
        for(i = 0; i < Nqueue; i ++) {
            int n = queue[i];
            P[n].DensityIterationDone = 0;
            Wind[n].DMRadius = 2 * P[n].Hsml;
            Wind[n].Left = 0;
            Wind[n].Right = -1;
        }
        int npleft = Nqueue;
        int done = 0;
        while(!done) {
            ev_run(&ev);
            for(i = 0; i < Nqueue; i ++) {
                int n = queue[i];
                if (P[n].DensityIterationDone) continue;
                int diff = Wind[n].Ngb - 40;
                if(diff < -2) {
                    /* too few */
                    Wind[n].Left = Wind[n].DMRadius;
                } else if(diff > 2) {
                    /* too many */
                    Wind[n].Right = Wind[n].DMRadius;
                } else {
                    P[n].DensityIterationDone = 1;
                    npleft --;
                }
                if(Wind[n].Right >= 0) {
                    /* if Ngb hasn't converged to 40, see if DMRadius converged*/
                    if(Wind[n].Right - Wind[n].Left < 1e-2) {
                        P[n].DensityIterationDone = 1;
                        npleft --;
                    } else {
                        Wind[n].DMRadius = 0.5 * (Wind[n].Left + Wind[n].Right);
                    }
                } else {
                    Wind[n].DMRadius *= 1.3;
                }
            }
            int64_t totalleft = 0;
            sumup_large_ints(1, &npleft, &totalleft);
            done = totalleft == 0;
            if(ThisTask == 0) {
                printf("Star DM iteration Total left = %ld\n", totalleft);
            }
        }
        for(i = 0; i < Nqueue; i ++) {
            int n = queue[i];
            double vdisp = Wind[n].V2sum / Wind[n].Ngb;
            int k;
            for(k = 0; k < 3; k ++) {
                Wind[n].Vmean[k] = Wind[n].V1sum[k] / Wind[n].Ngb;
                vdisp -= Wind[n].Vmean[k] * Wind[n].Vmean[k];
            }
            Wind[n].Vdisp = sqrt(vdisp / 3);
        }
        myfree(queue);
        ev.ev_evaluate = (ev_ev_func) sfr_wind_evaluate;
        ev.ev_reduce = NULL;

        ev_run(&ev);
        myfree(Wind);
    }
    walltime_measure("/Cooling/Wind");
#endif
}

static void cooling_direct(int i) {

    double dt = (P[i].TimeBin ? (1 << P[i].TimeBin) : 0) * All.Timebase_interval;
        /*  the actual time-step */

    double dtime;

    dtime = dt / All.cf.hubble;

    SPHP(i).Sfr = 0;

    double ne = SPHP(i).Ne;	/* electron abundance (gives ionization state and mean molecular weight) */

    double unew = DMAX(All.MinEgySpec,
            (SPHP(i).Entropy + SPHP(i).DtEntropy * dt) /
            GAMMA_MINUS1 * pow(SPHP(i).EOMDensity * All.cf.a3inv, GAMMA_MINUS1));

#ifdef BLACK_HOLES
    if(SPHP(i).Injected_BH_Energy)
    {
        if(P[i].Mass == 0)
            SPHP(i).Injected_BH_Energy = 0;
        else
            unew += SPHP(i).Injected_BH_Energy / P[i].Mass;

        double temp = u_to_temp_fac * unew;


        if(temp > 5.0e9)
            unew = 5.0e9 / u_to_temp_fac;

        SPHP(i).Injected_BH_Energy = 0;
    }
#endif

    struct UVBG uvbg;
    GetParticleUVBG(i, &uvbg);
    unew = DoCooling(unew, SPHP(i).Density * All.cf.a3inv, dtime, &uvbg, &ne, METALLICITY(i));

    SPHP(i).Ne = ne;

    if(P[i].TimeBin)	/* upon start-up, we need to protect against dt==0 */
    {
        /* note: the adiabatic rate has been already added in ! */

        if(dt > 0)
        {

            SPHP(i).DtEntropy = (unew * GAMMA_MINUS1 /
                    pow(SPHP(i).EOMDensity * All.cf.a3inv,
                        GAMMA_MINUS1) - SPHP(i).Entropy) / dt;

            if(SPHP(i).DtEntropy < -0.5 * SPHP(i).Entropy / dt)
                SPHP(i).DtEntropy = -0.5 * SPHP(i).Entropy / dt;
        }
    }
}

#endif /* closing of COOLING-conditional */


#if defined(SFR)


/* returns 0 if the particle is actively forming stars */
static int get_sfr_condition(int i) {
    int flag = 1;
/* no sfr !*/
    if(!All.StarformationOn) {
        return flag;
    }
    if(SPHP(i).Density * All.cf.a3inv >= All.PhysDensThresh)
        flag = 0;

    if(SPHP(i).Density < All.OverDensThresh)
        flag = 1;

#ifdef BLACK_HOLES
    if(P[i].Mass == 0)
        flag = 1;
#endif

#ifdef WINDS
    if(SPHP(i).DelayTime > 0)
        flag = 1;		/* only normal cooling for particles in the wind */
#endif

    if(All.QuickLymanAlphaProbability > 0) {
        double dt = (P[i].TimeBin ? (1 << P[i].TimeBin) : 0) * All.Timebase_interval;
        double unew = DMAX(All.MinEgySpec,
                (SPHP(i).Entropy + SPHP(i).DtEntropy * dt) /
                GAMMA_MINUS1 * pow(SPHP(i).EOMDensity * All.cf.a3inv, GAMMA_MINUS1));

        double temp = u_to_temp_fac * unew;

        if(SPHP(i).Density > All.OverDensThresh && temp < 1.0e5)
            flag = 0;
        else
            flag = 1;
    }

    return flag;
}

#ifdef WINDS
static int sfr_wind_isactive(int target) {
    if(P[target].Type == 4) {
        /*
         * protect beginning of time. StellarAge starts at 0.
         * */
        if(All.Time > 0 && P[target].StellarAge == All.Time) {
             return 1;
        }
    }
    return 0;
}

static void sfr_wind_reduce_weight(int place, struct winddata_out * O, int mode) {
    EV_REDUCE(Wind[place].TotalWeight, O->TotalWeight);
    int k;
    for(k = 0; k < 3; k ++) {
        EV_REDUCE(Wind[place].V1sum[k], O->V1sum[k]);
    }
    EV_REDUCE(Wind[place].V2sum, O->V2sum);
    EV_REDUCE(Wind[place].Ngb, O->Ngb);
    /*
    printf("Reduce ID=%ld, NGB=%d TotalWeight=%g V2sum=%g V1sum=%g %g %g\n",
            P[place].ID, O->Ngb, O->TotalWeight, O->V2sum,
            O->V1sum[0], O->V1sum[1], O->V1sum[2]);
            */
}

static void sfr_wind_copy(int place, struct winddata_in * input) {
    double dt = (P[place].TimeBin ? (1 << P[place].TimeBin) : 0) * All.Timebase_interval / All.cf.hubble;
    input->Dt = dt;
    int k;
    for (k = 0; k < 3; k ++)
        input->Pos[k] = P[place].Pos[k];
    input->Mass = P[place].Mass;
    input->Hsml = P[place].Hsml;
    input->TotalWeight = Wind[place].TotalWeight;
    input->ID = P[place].ID;

    input->DMRadius = Wind[place].DMRadius;
    input->Vdisp = Wind[place].Vdisp;
    for (k = 0; k < 3; k ++)
        input->Vmean[k] = Wind[place].Vmean[k];
}

static int sfr_wind_ev_weight(int target, int mode,
        struct winddata_in * I,
        struct winddata_out * O,
        LocalEvaluator * lv) {
    /* this evaluator walks the tree and sums the total mass of surrounding gas
     * particles as described in VS08. */
    int startnode, numngb, k, n, listindex = 0;
    startnode = I->NodeList[0];
    listindex ++;
    startnode = Nodes[startnode].u.d.nextnode;	/* open it */

    double hsearch = DMAX(I->Hsml, I->DMRadius);
    /*
    DensityKernel kernel;
    density_kernel_init(&kernel, I->Hsml);
    */
    while(startnode >= 0)
    {
        while(startnode >= 0)
        {
            numngb = ngb_treefind_threads(I->Pos, hsearch, target, &startnode,
                    mode, lv, NGB_TREEFIND_SYMMETRIC, 1 + 2);

            if(numngb < 0)
                return numngb;

            for(n = 0; n < numngb; n++)
            {
                int j = lv->ngblist[n];

                double dx = I->Pos[0] - P[j].Pos[0];
                double dy = I->Pos[1] - P[j].Pos[1];
                double dz = I->Pos[2] - P[j].Pos[2];

                dx = NEAREST(dx);
                dy = NEAREST(dy);
                dz = NEAREST(dz);
                double r2 = dx * dx + dy * dy + dz * dz;

                if(P[j].Type == 0) {
                    if(r2 > I->Hsml * I->Hsml) continue;
                    /* Ignore wind particles */
                    if(SPHP(j).DelayTime > 0) continue;
                    //double r = sqrt(r2);
                    //double wk = density_kernel_wk(&kernel, r);
                    double wk = 1.0;
                    O->TotalWeight += wk * P[j].Mass;
                }
                if(P[j].Type == 1) {
                    if(r2 > I->DMRadius * I->DMRadius) continue;
                    O->Ngb ++;
                    double d[3] = {dx, dy, dz};
                    for(k = 0; k < 3; k ++) {
                        double vel = P[j].Vel[k] + All.cf.hubble * All.cf.a * All.cf.a * d[k];
                        O->V1sum[k] += vel;
                        O->V2sum += vel * vel;
                    }
                }

            }
            /*
            printf("ThisTask = %d %ld ngb=%d NGB=%d TotalWeight=%g V2sum=%g V1sum=%g %g %g\n",
            ThisTask, I->ID, numngb, O->Ngb, O->TotalWeight, O->V2sum,
            O->V1sum[0], O->V1sum[1], O->V1sum[2]);
            */
        }
        if(listindex < NODELISTLENGTH)
        {
            startnode = I->NodeList[listindex];
            if(startnode >= 0) {
                startnode = Nodes[startnode].u.d.nextnode;	/* open it */
                listindex++;
            }
        }
    }


    return 0;


}
static int sfr_wind_evaluate(int target, int mode,
        struct winddata_in * I,
        struct winddata_out * O,
        LocalEvaluator * lv) {

    /* this evaluator walks the tree and blows wind. */

    int startnode, numngb, n, listindex = 0;
    startnode = I->NodeList[0];
    listindex ++;
    startnode = Nodes[startnode].u.d.nextnode;	/* open it */

    /*
    DensityKernel kernel;
    density_kernel_init(&kernel, I->Hsml);
    */
    while(startnode >= 0)
    {
        while(startnode >= 0)
        {
            numngb = ngb_treefind_threads(I->Pos, I->Hsml, target, &startnode,
                    mode, lv, NGB_TREEFIND_SYMMETRIC, 1);

            if(numngb < 0)
                return numngb;

            for(n = 0; n < numngb;
                    (unlock_particle_if_not(lv->ngblist[n], I->ID), n++)
                    )
            {
                lock_particle_if_not(lv->ngblist[n], I->ID);
                int j = lv->ngblist[n];
                /* skip wind particles */
                if(SPHP(j).DelayTime > 0) continue;

                double dx = I->Pos[0] - P[j].Pos[0];
                double dy = I->Pos[1] - P[j].Pos[1];
                double dz = I->Pos[2] - P[j].Pos[2];

                dx = NEAREST(dx);
                dy = NEAREST(dy);
                dz = NEAREST(dz);
                double r2 = dx * dx + dy * dy + dz * dz;
                if(r2 > I->Hsml * I->Hsml) continue;

                double windeff;
                double v;
                if(HAS(All.WindModel, WINDS_FIXED_EFFICIENCY)) {
                    windeff = All.WindEfficiency;
                    v = All.WindSpeed * All.cf.a;
                } else if(HAS(All.WindModel, WINDS_USE_HALO)) {
                    windeff = 1.0 / (I->Vdisp / All.cf.a / All.WindSigma0);
                    windeff *= windeff;
                    v = All.WindSpeedFactor * I->Vdisp;
                } else {
                    abort();
                }
                //double r = sqrt(r2);
                //double wk = density_kernel_wk(&kernel, r);
                double wk = 1.0;
                double p = windeff * wk * I->Mass / I->TotalWeight;
                double random = get_random_number(I->ID + P[j].ID);
                if (random < p) {
                    make_particle_wind(j, v, I->Vmean);
                }
            }
        }
        if(listindex < NODELISTLENGTH)
        {
            startnode = I->NodeList[listindex];
            if(startnode >= 0) {
                startnode = Nodes[startnode].u.d.nextnode;	/* open it */
                listindex++;
            }
        }
    }

    return 0;


}

static int make_particle_wind(int i, double v, double vmean[3]) {
    /* v and vmean are in internal units (km/s *a ), not km/s !*/
    /* returns 0 if particle i is converted to wind. */
    int j;
    /* ok, make the particle go into the wind */
    double dir[3];
    if(HAS(All.WindModel, WINDS_ISOTROPIC)) {
        double theta = acos(2 * get_random_number(P[i].ID + 3) - 1);
        double phi = 2 * M_PI * get_random_number(P[i].ID + 4);

        dir[0] = sin(theta) * cos(phi);
        dir[1] = sin(theta) * sin(phi);
        dir[2] = cos(theta);
    } else {
        double vel[3];
        for(j = 0; j < 3; j++) {
            vel[j] = P[i].Vel[j] - vmean[j];
        }
        dir[0] = P[i].GravAccel[1] * vel[2] - P[i].GravAccel[2] * vel[1];
        dir[1] = P[i].GravAccel[2] * vel[0] - P[i].GravAccel[0] * vel[2];
        dir[2] = P[i].GravAccel[0] * vel[1] - P[i].GravAccel[1] * vel[0];
    }

    double norm = 0;
    for(j = 0; j < 3; j++)
        norm += dir[j] * dir[j];

    norm = sqrt(norm);
    if(get_random_number(P[i].ID + 5) < 0.5)
        norm = -norm;

    if(norm != 0)
    {
        for(j = 0; j < 3; j++)
            dir[j] /= norm;

        for(j = 0; j < 3; j++)
        {
            P[i].Vel[j] += v * dir[j];
            SPHP(i).VelPred[j] += v * dir[j];
        }
        SPHP(i).DelayTime = All.WindFreeTravelLength / (v / All.cf.a);
    }
    return 0;
}
#endif

static int make_particle_star(int i) {

    /* here we spawn a new star particle */
    double mass_of_star =  All.MassTable[0] / GENERATIONS;

    /* ok, make a star */
    if(P[i].Mass < 1.1 * mass_of_star || All.QuickLymanAlphaProbability > 0)
    {
        /* here we turn the gas particle itself into a star */
        stars_converted++;
        N_star++;
        sum_mass_stars += P[i].Mass;

        P[i].Type = 4;
        TimeBinCountSph[P[i].TimeBin]--;

#ifdef WINDS
        P[i].StellarAge = All.Time;
#endif
    }
    else
    {
        int child = domain_fork_particle(i);

        N_star++;
        /* set ptype */
        P[child].Type = 4;
        P[child].Mass = mass_of_star;
        P[i].Mass -= P[child].Mass;
        sum_mass_stars += P[child].Mass;
#ifdef WINDS
        P[child].StellarAge = All.Time;
#endif
        stars_spawned++;
    }
    return 0;
}

static void cooling_relaxed(int i, double egyeff, double dtime, double trelax) {
    const double densityfac = pow(SPHP(i).EOMDensity * All.cf.a3inv, GAMMA_MINUS1) / GAMMA_MINUS1;
    double egycurrent = SPHP(i).Entropy *  densityfac;

#ifdef BLACK_HOLES
    if(SPHP(i).Injected_BH_Energy > 0)
    {
        struct UVBG uvbg;
        GetParticleUVBG(i, &uvbg);
        egycurrent += SPHP(i).Injected_BH_Energy / P[i].Mass;

        double temp = u_to_temp_fac * egycurrent;

        if(temp > 5.0e9)
            egycurrent = 5.0e9 / u_to_temp_fac;

        if(egycurrent > egyeff)
        {
            double ne = SPHP(i).Ne;
            double tcool = GetCoolingTime(egycurrent, SPHP(i).Density * All.cf.a3inv, &uvbg, &ne, P[i].Metallicity);

            if(tcool < trelax && tcool > 0)
                trelax = tcool;
        }

        SPHP(i).Injected_BH_Energy = 0;
    }
#endif

    SPHP(i).Entropy =  (egyeff + (egycurrent - egyeff) * exp(-dtime / trelax)) /densityfac;

    SPHP(i).DtEntropy = 0;

}

static void starformation(int i) {

    double mass_of_star = All.MassTable[0] / GENERATIONS;

    double dt = (P[i].TimeBin ? (1 << P[i].TimeBin) : 0) * All.Timebase_interval;
        /*  the actual time-step */

    double dtime = dt / All.cf.hubble;

    double egyeff, trelax;
    double rateOfSF = get_starformation_rate_full(i, dtime, &SPHP(i).Ne, &trelax, &egyeff);

    /* amount of stars expect to form */

    double sm = rateOfSF * dtime;

    double p = sm / P[i].Mass;

    sum_sm += P[i].Mass * (1 - exp(-p));

    /* convert to Solar per Year but is this damn variable otherwise used
     * at all? */
    SPHP(i).Sfr = rateOfSF *
        (All.UnitMass_in_g / SOLAR_MASS) / (All.UnitTime_in_s / SEC_PER_YEAR);

#ifdef METALS
    double w = get_random_number(P[i].ID);
    P[i].Metallicity += w * METAL_YIELD * (1 - exp(-p));
#endif

    if(dt > 0 && P[i].TimeBin)
    {
      	/* upon start-up, we need to protect against dt==0 */
        cooling_relaxed(i, egyeff, dtime, trelax);
    }

    double prob = P[i].Mass / mass_of_star * (1 - exp(-p));

    if(All.QuickLymanAlphaProbability > 0.0) {
        prob = All.QuickLymanAlphaProbability;
    }
    if(get_random_number(P[i].ID + 1) < prob)	{
#pragma omp critical (_sfr_)
        make_particle_star(i);
    }

    if(P[i].Type == 0)	{
    /* to protect using a particle that has been turned into a star */
#ifdef METALS
        P[i].Metallicity += (1 - w) * METAL_YIELD * (1 - exp(-p));
#endif
#ifdef WINDS
        if(HAS(All.WindModel, WINDS_SUBGRID)) {
            /* Here comes the Springel Hernquist 03 wind model */
            double pw = All.WindEfficiency * sm / P[i].Mass;
            double prob = 1 - exp(-pw);
            double zero[3] = {0, 0, 0};
            if(get_random_number(P[i].ID + 2) < prob)
                make_particle_wind(i, All.WindSpeed * All.cf.a, zero);
        }
#endif
    }


}

double get_starformation_rate(int i) {
    /* returns SFR in internal units */
    return get_starformation_rate_full(i, 0, NULL, NULL, NULL);
}

static double get_starformation_rate_full(int i, double dtime, double * ne_new, double * trelax, double * egyeff) {
    double rateOfSF;
    int flag;
    double tsfr;
    double factorEVP, egyhot, ne, tcool, y, x, cloudmass;
    struct UVBG uvbg;

    flag = get_sfr_condition(i);

    if(flag == 1) {
        /* this shall not happen but let's put in some safe
         * numbers in case the code run wary!
         *
         * the only case trelax and egyeff are
         * required is in starformation(i)
         * */
        if (trelax) {
            *trelax = All.MaxSfrTimescale;
        }
        if (egyeff) {
            *egyeff = All.EgySpecCold;
        }
        return 0;
    }

    tsfr = sqrt(All.PhysDensThresh / (SPHP(i).Density * All.cf.a3inv)) * All.MaxSfrTimescale;
    /*
     * gadget-p doesn't have this cap.
     * without the cap sm can be bigger than cloudmass.
    */
    if(tsfr < dtime)
        tsfr = dtime;

    GetParticleUVBG(i, &uvbg);

    factorEVP = pow(SPHP(i).Density * All.cf.a3inv / All.PhysDensThresh, -0.8) * All.FactorEVP;

    egyhot = All.EgySpecSN / (1 + factorEVP) + All.EgySpecCold;

    ne = SPHP(i).Ne;

    tcool = GetCoolingTime(egyhot, SPHP(i).Density * All.cf.a3inv, &uvbg, &ne, METALLICITY(i));
    y = tsfr / tcool * egyhot / (All.FactorSN * All.EgySpecSN - (1 - All.FactorSN) * All.EgySpecCold);

    x = 1 + 1 / (2 * y) - sqrt(1 / y + 1 / (4 * y * y));

    cloudmass = x * P[i].Mass;

    rateOfSF = (1 - All.FactorSN) * cloudmass / tsfr;

    if (ne_new ) {
        *ne_new = ne;
    }

    if (trelax) {
        *trelax = tsfr * (1 - x) / x / (All.FactorSN * (1 + factorEVP));
    }
    if (egyeff) {
        *egyeff = egyhot * (1 - x) + All.EgySpecCold * x;
    }

    if (HAS(All.StarformationCriterion, SFR_CRITERION_MOLECULAR_H2)) {
        rateOfSF *= get_sfr_factor_due_to_h2(i);
    }
    if (HAS(All.StarformationCriterion, SFR_CRITERION_SELFGRAVITY)) {
        rateOfSF *= get_sfr_factor_due_to_selfgravity(i);
    }
    return rateOfSF;
}

void init_clouds(void)
{
    double A0, dens, tcool, ne, coolrate, egyhot, x, u4, meanweight;
    double tsfr, y, peff, fac, neff, egyeff, factorEVP, sigma, thresholdStarburst;

    if(All.PhysDensThresh == 0)
    {
        A0 = All.FactorEVP;

        egyhot = All.EgySpecSN / A0;

        meanweight = 4 / (8 - 5 * (1 - HYDROGEN_MASSFRAC));	/* note: assuming FULL ionization */

        u4 = 1 / meanweight * (1.0 / GAMMA_MINUS1) * (BOLTZMANN / PROTONMASS) * 1.0e4;
        u4 *= All.UnitMass_in_g / All.UnitEnergy_in_cgs;


        dens = 1.0e6 * 3 * All.Hubble * All.Hubble / (8 * M_PI * All.G);

        /* to be guaranteed to get z=0 rate */
        set_global_time(1.0);
        IonizeParams();

        ne = 1.0;

        SetZeroIonization();
        struct UVBG uvbg;
        GetGlobalUVBG(&uvbg);
        /*XXX: We set the threshold without metal cooling;
         * It probably make sense to set the parameters with
         * a metalicity dependence.
         * */
        tcool = GetCoolingTime(egyhot, dens, &uvbg, &ne, 0.0);

        coolrate = egyhot / tcool / dens;

        x = (egyhot - u4) / (egyhot - All.EgySpecCold);

        All.PhysDensThresh =
            x / pow(1 - x,
                    2) * (All.FactorSN * All.EgySpecSN - (1 -
                            All.FactorSN) * All.EgySpecCold) /
                        (All.MaxSfrTimescale * coolrate);

        if(ThisTask == 0)
        {
            printf("\nA0= %g  \n", A0);
            printf("Computed: PhysDensThresh= %g  (int units)         %g h^2 cm^-3\n", All.PhysDensThresh,
                    All.PhysDensThresh / (PROTONMASS / HYDROGEN_MASSFRAC / All.UnitDensity_in_cgs));
            printf("EXPECTED FRACTION OF COLD GAS AT THRESHOLD = %g\n\n", x);
            printf("tcool=%g dens=%g egyhot=%g\n", tcool, dens, egyhot);
        }

        dens = All.PhysDensThresh * 10;

        do
        {
            tsfr = sqrt(All.PhysDensThresh / (dens)) * All.MaxSfrTimescale;
            factorEVP = pow(dens / All.PhysDensThresh, -0.8) * All.FactorEVP;
            egyhot = All.EgySpecSN / (1 + factorEVP) + All.EgySpecCold;

            ne = 0.5;
            tcool = GetCoolingTime(egyhot, dens, &uvbg, &ne, 0.0);

            y = tsfr / tcool * egyhot / (All.FactorSN * All.EgySpecSN - (1 - All.FactorSN) * All.EgySpecCold);
            x = 1 + 1 / (2 * y) - sqrt(1 / y + 1 / (4 * y * y));
            egyeff = egyhot * (1 - x) + All.EgySpecCold * x;

            peff = GAMMA_MINUS1 * dens * egyeff;

            fac = 1 / (log(dens * 1.025) - log(dens));
            dens *= 1.025;

            neff = -log(peff) * fac;

            tsfr = sqrt(All.PhysDensThresh / (dens)) * All.MaxSfrTimescale;
            factorEVP = pow(dens / All.PhysDensThresh, -0.8) * All.FactorEVP;
            egyhot = All.EgySpecSN / (1 + factorEVP) + All.EgySpecCold;

            ne = 0.5;
            tcool = GetCoolingTime(egyhot, dens, &uvbg, &ne, 0.0);

            y = tsfr / tcool * egyhot / (All.FactorSN * All.EgySpecSN - (1 - All.FactorSN) * All.EgySpecCold);
            x = 1 + 1 / (2 * y) - sqrt(1 / y + 1 / (4 * y * y));
            egyeff = egyhot * (1 - x) + All.EgySpecCold * x;

            peff = GAMMA_MINUS1 * dens * egyeff;

            neff += log(peff) * fac;
        }
        while(neff > 4.0 / 3);

        thresholdStarburst = dens;

        if(ThisTask == 0)
        {
            printf("Run-away sets in for dens=%g\n", thresholdStarburst);
            printf("Dynamic range for quiescent star formation= %g\n", thresholdStarburst / All.PhysDensThresh);
            fflush(stdout);
        }

        integrate_sfr();

        if(ThisTask == 0)
        {
            sigma = 10.0 / All.Hubble * 1.0e-10 / pow(1.0e-3, 2);

            printf("Isotherm sheet central density: %g   z0=%g\n",
                    M_PI * All.G * sigma * sigma / (2 * GAMMA_MINUS1) / u4,
                    GAMMA_MINUS1 * u4 / (2 * M_PI * All.G * sigma));
            fflush(stdout);

        }

        set_global_time(All.TimeBegin);
        IonizeParams();
    }
}

void integrate_sfr(void)
{
    double rho0, rho, rho2, q, dz, gam, sigma = 0, sigma_u4, sigmasfr = 0, ne, P1;
    double x = 0, y, P, P2, x2, y2, tsfr2, egyhot2, tcool2, drho, dq;
    double meanweight, u4, tsfr, tcool, egyhot, factorEVP, egyeff, egyeff2;
    FILE *fd;

    meanweight = 4 / (8 - 5 * (1 - HYDROGEN_MASSFRAC));	/* note: assuming FULL ionization */
    u4 = 1 / meanweight * (1.0 / GAMMA_MINUS1) * (BOLTZMANN / PROTONMASS) * 1.0e4;
    u4 *= All.UnitMass_in_g / All.UnitEnergy_in_cgs;

    /* to be guaranteed to get z=0 rate */
    set_global_time(1.0);
    IonizeParams();

    struct UVBG uvbg;
    GetGlobalUVBG(&uvbg);


    if(ThisTask == 0)
        fd = fopen("eos.txt", "w");
    else
        fd = 0;

    for(rho = All.PhysDensThresh; rho <= 1e7 * All.PhysDensThresh; rho *= 2)
    {
        tsfr = sqrt(All.PhysDensThresh / rho) * All.MaxSfrTimescale;

        factorEVP = pow(rho / All.PhysDensThresh, -0.8) * All.FactorEVP;

        egyhot = All.EgySpecSN / (1 + factorEVP) + All.EgySpecCold;

        ne = 1.0;
        tcool = GetCoolingTime(egyhot, rho, &uvbg, &ne, 0.0);

        y = tsfr / tcool * egyhot / (All.FactorSN * All.EgySpecSN - (1 - All.FactorSN) * All.EgySpecCold);
        x = 1 + 1 / (2 * y) - sqrt(1 / y + 1 / (4 * y * y));

        egyeff = egyhot * (1 - x) + All.EgySpecCold * x;

        P = GAMMA_MINUS1 * rho * egyeff;

        if(ThisTask == 0)
        {
            fprintf(fd, "%g %g %g %g %g\n", rho, P, x, tcool, egyhot);
        }
    }

    if(ThisTask == 0)
        fclose(fd);


    if(ThisTask == 0)
        fd = fopen("sfrrate.txt", "w");
    else
        fd = 0;

    for(rho0 = All.PhysDensThresh; rho0 <= 10000 * All.PhysDensThresh; rho0 *= 1.02)
    {
        rho = rho0;
        q = 0;
        dz = 0.001;

        sigma = sigmasfr = sigma_u4 = 0;

        while(rho > 0.0001 * rho0)
        {
            if(rho > All.PhysDensThresh)
            {
                tsfr = sqrt(All.PhysDensThresh / rho) * All.MaxSfrTimescale;

                factorEVP = pow(rho / All.PhysDensThresh, -0.8) * All.FactorEVP;

                egyhot = All.EgySpecSN / (1 + factorEVP) + All.EgySpecCold;

                ne = 1.0;
                tcool = GetCoolingTime(egyhot, rho, &uvbg, &ne, 0.0);

                y =
                    tsfr / tcool * egyhot / (All.FactorSN * All.EgySpecSN - (1 - All.FactorSN) * All.EgySpecCold);
                x = 1 + 1 / (2 * y) - sqrt(1 / y + 1 / (4 * y * y));

                egyeff = egyhot * (1 - x) + All.EgySpecCold * x;

                P = P1 = GAMMA_MINUS1 * rho * egyeff;

                rho2 = 1.1 * rho;
                tsfr2 = sqrt(All.PhysDensThresh / rho2) * All.MaxSfrTimescale;
                egyhot2 = All.EgySpecSN / (1 + factorEVP) + All.EgySpecCold;
                tcool2 = GetCoolingTime(egyhot2, rho2, &uvbg, &ne, 0.0);
                y2 =
                    tsfr2 / tcool2 * egyhot2 / (All.FactorSN * All.EgySpecSN -
                            (1 - All.FactorSN) * All.EgySpecCold);
                x2 = 1 + 1 / (2 * y2) - sqrt(1 / y2 + 1 / (4 * y2 * y2));
                egyeff2 = egyhot2 * (1 - x2) + All.EgySpecCold * x2;
                P2 = GAMMA_MINUS1 * rho2 * egyeff2;

                gam = log(P2 / P1) / log(rho2 / rho);
            }
            else
            {
                tsfr = 0;

                P = GAMMA_MINUS1 * rho * u4;
                gam = 1.0;


                sigma_u4 += rho * dz;
            }



            drho = q;
            dq = -(gam - 2) / rho * q * q - 4 * M_PI * All.G / (gam * P) * rho * rho * rho;

            sigma += rho * dz;
            if(tsfr > 0)
            {
                sigmasfr += (1 - All.FactorSN) * rho * x / tsfr * dz;
            }

            rho += drho * dz;
            q += dq * dz;
        }


        sigma *= 2;		/* to include the other side */
        sigmasfr *= 2;
        sigma_u4 *= 2;


        if(ThisTask == 0)
        {
            fprintf(fd, "%g %g %g %g\n", rho0, sigma, sigmasfr, sigma_u4);
        }
    }


    set_global_time(All.TimeBegin);
    IonizeParams();

    if(ThisTask == 0)
        fclose(fd);
}

void set_units_sfr(void)
{

    double meanweight;

    All.OverDensThresh =
        All.CritOverDensity * All.OmegaBaryon * 3 * All.Hubble * All.Hubble / (8 * M_PI * All.G);

    All.PhysDensThresh = All.CritPhysDensity * PROTONMASS / HYDROGEN_MASSFRAC / All.UnitDensity_in_cgs;

    meanweight = 4 / (1 + 3 * HYDROGEN_MASSFRAC);	/* note: assuming NEUTRAL GAS */

    All.EgySpecCold = 1 / meanweight * (1.0 / GAMMA_MINUS1) * (BOLTZMANN / PROTONMASS) * All.TempClouds;
    All.EgySpecCold *= All.UnitMass_in_g / All.UnitEnergy_in_cgs;

    meanweight = 4 / (8 - 5 * (1 - HYDROGEN_MASSFRAC));	/* note: assuming FULL ionization */

    All.EgySpecSN = 1 / meanweight * (1.0 / GAMMA_MINUS1) * (BOLTZMANN / PROTONMASS) * All.TempSupernova;
    All.EgySpecSN *= All.UnitMass_in_g / All.UnitEnergy_in_cgs;

    if(HAS(All.WindModel, WINDS_FIXED_EFFICIENCY)) {
        All.WindSpeed = sqrt(2 * All.WindEnergyFraction * All.FactorSN * All.EgySpecSN / (1 - All.FactorSN) / All.WindEfficiency);
        if(ThisTask == 0)
                printf("Windspeed: %g\n", All.WindSpeed);
    } else {
        All.WindSpeed = sqrt(2 * All.WindEnergyFraction * All.FactorSN * All.EgySpecSN / (1 - All.FactorSN) / 1.0);
        if(ThisTask == 0 && All.WindModel != WINDS_NONE)
                printf("Reference Windspeed: %g\n", All.WindSigma0 * All.WindSpeedFactor);

    }

}
/********************
 *
 * The follow functions are from Desika and Gadget-P.
 * We really are mostly concerned about H2 here.
 *
 * You may need a license to run with these modess.
 
 * */
#if defined SPH_GRAD_RHO && defined METALS
static double ev_NH_from_GradRho(MyFloat gradrho[3], double hsml, double rho, double include_h)
{
    /* column density from GradRho, copied from gadget-p; what is it
     * calculating? */
    double gradrho_mag;
    if(rho<=0) {
        gradrho_mag = 0;
    } else {
        gradrho_mag = sqrt(gradrho[0]*gradrho[0]+gradrho[1]*gradrho[1]+gradrho[2]*gradrho[2]);
        if(gradrho_mag > 0) {gradrho_mag = rho*rho/gradrho_mag;} else {gradrho_mag=0;}
        if(include_h > 0) gradrho_mag += include_h*rho*hsml;
    }
    return gradrho_mag; // *(Z/Zsolar) add metallicity dependence
}
#endif

static double get_sfr_factor_due_to_h2(int i) {
    /*  Krumholz & Gnedin fitting function for f_H2 as a function of local
     *  properties, from gadget-p; we return the enhancement on SFR in this
     *  function */

#if ! defined SPH_GRAD_RHO || ! defined METALS
    /* if SPH_GRAD_RHO is not enabled, disable H2 molecular gas
     * this really shall not happen because begrun will check against the
     * condition. Ditto if not metal tracking.
     * */
    return 1.0;
#else
    double tau_fmol;
    double zoverzsun = P[i].Metallicity/METAL_YIELD;
    tau_fmol = ev_NH_from_GradRho(SPHP(i).GradRho,P[i].Hsml,SPHP(i).Density,1) * All.cf.a2inv;
    tau_fmol *= (0.1 + zoverzsun);
    if(tau_fmol>0) {
        tau_fmol *= 434.78*All.UnitDensity_in_cgs*All.HubbleParam*All.UnitLength_in_cm;
        double y = 0.756*(1+3.1*pow(zoverzsun,0.365));
        y = log(1+0.6*y+0.01*y*y)/(0.6*tau_fmol);
        y = 1-0.75*y/(1+0.25*y);
        if(y<0) y=0; if(y>1) y=1;
        return y;

    } // if(tau_fmol>0)
    return 1.0;
#endif
}

static double get_sfr_factor_due_to_selfgravity(int i) {
#ifdef SPH_GRAD_RHO
    double divv = SPHP(i).DivVel * All.cf.a2inv;

    divv += 3.0*All.cf.hubble_a2; // hubble-flow correction

    if(HAS(All.StarformationCriterion, SFR_CRITERION_CONVERGENT_FLOW)) {
        if( divv>=0 ) return 0; // restrict to convergent flows (optional) //
    }

    double dv2abs = (divv*divv
            + (SPHP(i).CurlVel*All.cf.a2inv)
            * (SPHP(i).CurlVel*All.cf.a2inv)
           ); // all in physical units
    double alpha_vir = 0.2387 * dv2abs/(All.G * SPHP(i).Density*All.cf.a3inv);

    double y = 1.0;

    if((alpha_vir < 1.0)
    || (SPHP(i).Density * All.cf.a3inv > 100. * All.PhysDensThresh)
    )  {
        y = 66.7;
    } else {
        y = 0.1;
    }
    // PFH: note the latter flag is an arbitrary choice currently set
    // -by hand- to prevent runaway densities from this prescription! //

    if (HAS(All.StarformationCriterion, SFR_CRITERION_CONTINUOUS_CUTOFF)) {
        // continuous cutoff w alpha_vir instead of sharp (optional) //
        y *= 1.0/(1.0 + alpha_vir);
    }
    return y;
#else
    return 1.0;
#endif
}

#endif /* closes COOLING */

