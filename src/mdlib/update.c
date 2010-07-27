/* -*- mode: c; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; c-file-style: "stroustrup"; -*-
 *
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * GROwing Monsters And Cloning Shrimps
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif


#include <stdio.h>
#include <math.h>

#include "sysstuff.h"
#include "smalloc.h"
#include "typedefs.h"
#include "nrnb.h"
#include "physics.h"
#include "macros.h"
#include "vec.h"
#include "main.h"
#include "confio.h"
#include "update.h"
#include "gmx_random.h"
#include "futil.h"
#include "mshift.h"
#include "tgroup.h"
#include "force.h"
#include "names.h"
#include "txtdump.h"
#include "mdrun.h"
#include "copyrite.h"
#include "constr.h"
#include "edsam.h"
#include "pull.h"
#include "disre.h"
#include "orires.h"
#include "gmx_wallcycle.h"
#include "3dview.h"
#include "bondf.h"

typedef struct {
  double gdt;
  double eph;
  double emh;
  double em;
  double b;
  double c;
  double d;
} gmx_sd_const_t;

typedef struct {
  real V;
  real X;
  real Yv;
  real Yx;
} gmx_sd_sigma_t;

typedef struct {
  /* The random state */
  gmx_rng_t gaussrand;
  /* BD stuff */
  real *bd_rf;
  /* SD stuff */
  gmx_sd_const_t *sdc;
  gmx_sd_sigma_t *sdsig;
  rvec *sd_V;
  int  sd_V_nalloc;
} gmx_stochd_t;

typedef struct gmx_update
{
    gmx_stochd_t *sd;
    rvec *xp;
    int  xp_nalloc;
    /* Variables for the deform algorithm */
    gmx_step_t deformref_step;
    matrix     deformref_box;
} t_gmx_update;


void rand_rot_mc(rvec x,vec4 xrot,
                     rvec delta,rvec xcm)
{
  mat4 mt1,mt2,mr[DIM],mtemp1,mtemp2,mtemp3,mxtot,mvtot;
  real phi;
  int  i,m;
  
  
  translate(-xcm[XX],-xcm[YY],-xcm[ZZ],mt1);  /* move c.o.ma to origin */

  for(m=0; (m<DIM); m++) {
    rotate(m,delta[m],mr[m]);
  }

  translate(xcm[XX],xcm[YY],xcm[ZZ],mt2);

  /* For mult_matrix we need to multiply in the opposite order
   * compared to normal mathematical notation.
   */
  mult_matrix(mtemp1,mt1,mr[XX]);
  mult_matrix(mtemp2,mr[YY],mr[ZZ]);
  mult_matrix(mtemp3,mtemp1,mtemp2);
  mult_matrix(mxtot,mtemp3,mt2);
  mult_matrix(mvtot,mr[XX],mtemp2);
  
  m4_op(mxtot,x,xrot);
}
void stretch_bonds(rvec *x,gmx_mc_move *mc_move,t_graph *graph)
{
  int ai,aj,ak,nr,list_r[200],*list;
  int k,m;
  rvec r1,r_ij,u1,v;

     ai = mc_move->group[MC_BONDS].ai;
     aj = mc_move->group[MC_BONDS].aj;
     nr = 0;
     bond_rot(graph,ai,aj,list_r,&nr,-1);
     list=list_r;
     copy_rvec(x[aj],r1);
     rvec_sub(x[aj],x[ai],r_ij);
     unitv(r_ij,u1);

     svmul(mc_move->group[MC_BONDS].value,u1,v);
     
     rvec_add(x[aj],v,x[aj]);
     for(k=0;k<nr;k++) {
      ak=list[k];
      rvec_add(x[ak],v,x[ak]);
     }

}
void rotate_dihedral(rvec *x,gmx_mc_move *mc_move,t_graph *graph)
{
  int    n,i,k,start,end;
  int    ai,aj,ak,nr,list_r[400],*list;
  rvec   r_ij,r_kj,r1,r2,u1,u2,u3;
  vec4 xrot;
  rvec xcm;
  matrix basis,basis_inv;
  rvec delta_phi;
     ai = mc_move->group[MC_DIHEDRALS].ai;
     aj = mc_move->group[MC_DIHEDRALS].aj;

     nr = 0;
     bond_rot(graph,ai,aj,list_r,&nr,-1);

     list = list_r;

     ak=list[0];
     rvec_sub(x[aj],x[ai],r_ij);
     rvec_sub(x[aj],x[ak],r_kj);
     cprod(r_ij,r_kj,r1);
     cprod(r_ij,r1,r2);

     unitv(r_ij,u1);
     unitv(r1,u2);
     unitv(r2,u3);
    
     basis[XX][XX] = u1[XX]; basis[XX][YY] = u2[XX]; basis[XX][ZZ] = u3[XX]; 
     basis[YY][XX] = u1[YY]; basis[YY][YY] = u2[YY]; basis[YY][ZZ] = u3[YY]; 
     basis[ZZ][XX] = u1[ZZ]; basis[ZZ][YY] = u2[ZZ]; basis[ZZ][ZZ] = u3[ZZ]; 

     basis_inv[XX][XX] = basis[XX][XX]; basis_inv[XX][YY] = basis[YY][XX]; basis_inv[XX][ZZ] = basis[ZZ][XX]; 
     basis_inv[YY][XX] = basis[XX][YY]; basis_inv[YY][YY] = basis[YY][YY]; basis_inv[YY][ZZ] = basis[ZZ][YY]; 
     basis_inv[ZZ][XX] = basis[XX][ZZ]; basis_inv[ZZ][YY] = basis[YY][ZZ]; basis_inv[ZZ][ZZ] = basis[ZZ][ZZ]; 


     clear_rvec(xcm);
     clear_rvec(delta_phi);
     delta_phi[XX]=mc_move->group[MC_DIHEDRALS].value;

     
     for(k=0;k<nr;k++) {
      ak=list[k];
      rvec_sub(x[ak],x[aj],r_kj);
      mvmul(basis_inv,r_kj,r1);
      rand_rot_mc(r1,xrot,delta_phi,xcm);
      for(i=0;i<DIM;i++)
       r1[i]=xrot[i];
      mvmul(basis,r1,r2);
      rvec_add(x[aj],r2,x[ak]);
     }
}

void cholesky(real **a,real **b,real **c,real coef,int n1,int n)
{
 int i,j,k;
 real sum;

 for(i=0;i<n;i++)
 {
  for(j=0;j<n;j++)
  {
   b[i][j]=c[i][j]=0;
  }
 }
 
 for(i=0;i<n;i++)
 {
  for(j=i;j<n;j++)
  {
   for (sum=a[i][j],k=i-1;k>=0;k--)
   {
    sum -= b[i][k]*b[j][k];
   }
   if(i == j)
   {
    if (sum <= 0.0)
    {
     gmx_fatal(FARGS,"Cholesky decomposition in CRA movement failed");
    }
    b[i][i]=sqrt(sum);
    c[i][i]=b[i][i];
   }
   else
   {
    b[j][i]=sum/b[i][i];
    c[i][j]=b[j][i];
   }
  }
 }
 for(i=n1;i<n;i++)
 {
  for(j=i;j<n;j++)
  {
   c[i][j] = c[i][j]*coef;
  }
 }
 /*sum=0;
 for(i=0;i<n;i++)
 {
  sum += b[11][i]*c[i][15];
 }
 printf("sum %f %f\n",sum,a[11][15]);*/
}
void mk_basis(matrix basis,matrix basis_inv,rvec u1,rvec u2,rvec u3)
{
     basis[XX][XX] = u1[XX]; basis[XX][YY] = u2[XX]; basis[XX][ZZ] = u3[XX]; 
     basis[YY][XX] = u1[YY]; basis[YY][YY] = u2[YY]; basis[YY][ZZ] = u3[YY]; 
     basis[ZZ][XX] = u1[ZZ]; basis[ZZ][YY] = u2[ZZ]; basis[ZZ][ZZ] = u3[ZZ]; 

     basis_inv[XX][XX] = basis[XX][XX]; basis_inv[XX][YY] = basis[YY][XX]; basis_inv[XX][ZZ] = basis[ZZ][XX]; 
     basis_inv[YY][XX] = basis[XX][YY]; basis_inv[YY][YY] = basis[YY][YY]; basis_inv[YY][ZZ] = basis[ZZ][YY]; 
     basis_inv[ZZ][XX] = basis[XX][ZZ]; basis_inv[ZZ][YY] = basis[YY][ZZ]; basis_inv[ZZ][ZZ] = basis[ZZ][ZZ]; 
}
void mk_rot(rvec xaa,rvec xaj,rvec xprimeaa,rvec xcm,rvec delta_phi,matrix basis,matrix basis_inv)
{
 rvec r_aaj,r1,r2;
 vec4 xrot;
 int  i;

       rvec_sub(xaa,xaj,r_aaj);
       mvmul(basis_inv,r_aaj,r1);
       rand_rot_mc(r1,xrot,delta_phi,xcm);
       for(i=0;i<DIM;i++)
        r1[i]=xrot[i];
       mvmul(basis,r1,r2);
       rvec_add(xaj,r2,xprimeaa);
}

void mk_cra_angle_list(int *angle_i,int *angle_j,int *angle_k,gmx_mc_move *mc_move,int jj)
{
     angle_i[0] = (mc_move->group[MC_CRA].ilist)->iatoms[jj];
     angle_j[0] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+1];
     angle_k[0] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+2];
     angle_i[1] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+1];
     angle_j[1] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+2];
     angle_k[1] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+3];
     angle_i[2] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+2];
     angle_j[2] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+3];
     angle_k[2] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+4];
     angle_i[3] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+3];
     angle_j[3] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+4];
     angle_k[3] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+5];
     angle_i[4] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+4];
     angle_j[4] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+5];
     angle_k[4] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+6];
     angle_i[5] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+5];
     angle_j[5] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+6];
     angle_k[5] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+7];
     angle_i[6] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+6];
     angle_j[6] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+7];
     angle_k[6] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+8];
     angle_i[7] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+7];
     angle_j[7] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+8];
     angle_k[7] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+9];
     angle_i[8] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+8];
     angle_j[8] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+9];
     angle_k[8] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+10];
     angle_i[9] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+9];
     angle_j[9] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+10];
     angle_k[9] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+11];
}
void  chi_to_psi(real ** matrix_lt,gmx_rng_t rng,real *delta_chi,real *delta_psi,int nn)
{
 int i,j;
 real gauss,sum;

 for(i=nn-1;i>=0;i--)
 {
  sum=0;
  gauss = gmx_rng_gaussian_real(rng);
  delta_chi[i] = gauss;
  for(j=i+1;j<nn;j++)
  {
   sum+=matrix_lt[i][j]*delta_psi[j];
  }
   delta_psi[i] = (gauss-sum)/matrix_lt[i][i];
 }
}
void psi_to_chi(real ** matrix_lt,real *delta_chi,real *delta_psi,int nn)
{
 int i,j;
 for(i=0;i<nn;i++)
 {
  delta_chi[i] = 0;
  for (j=0;j<nn;j++)
  {
   delta_chi[i] += matrix_lt[i][j]*delta_psi[j];
  }
 }
}
real bias_prob(real **matrix,real *delta_chi,int nn)
{
 int ii;
 real det=1,d2=0,prob;
  for(ii=0;ii<nn;ii++)
  {
   det *= matrix[ii][ii];
   d2 += delta_chi[ii]*delta_chi[ii];
  }
  prob = det*exp(-d2);
 return prob;
}
void eval_r(real omega1,matrix t0inv,matrix r1inv,rvec sq0,rvec r)
{
 rvec r1;
 real sin_o = sin(omega1);
 real cos_o = cos(omega1);

 r1inv[0][0] =  1;    r1inv[0][1] = 0;       r1inv[0][2] = 0;
 r1inv[1][0] =  0;    r1inv[1][1] = cos_o;   r1inv[1][2] = sin_o;
 r1inv[2][0] =  0;    r1inv[2][1] = -sin_o;  r1inv[2][2] = cos_o;

 mvmul(t0inv,sq0,r1);
 mvmul(r1inv,r1,r);
}

void eval_t1invq1(rvec r,matrix t1,rvec q1,real p2x,real p3x,bool first)
{
 real r2,rx2,ry2,p2x2,p3x2;
 real w,u,v;
 real sin_a,cos_a;

 p2x2 = sqr(p2x);
 p3x2 = sqr(p3x);
 r2 = sqr(norm(r));
 rx2 = sqr(r[0]);
 ry2 = sqr(r[1]);

 w = r2 + p2x2 - p3x2; 

 u = 4*p2x2*(rx2+ry2)-sqr(w);
 u = sqrt(4*p2x2*(rx2+ry2)-sqr(w));
 v = (1/(2*p2x*(rx2+ry2)));

 if(first) 
 {
  sin_a = v*(w*r[1]-r[0]*u);
  cos_a = v*(w*r[0]+r[1]*u);
 }
 else 
 {
  sin_a = v*(w*r[1]+r[0]*u);
  cos_a = v*(w*r[0]-r[1]*u);
 }

 t1[0][0] =  cos_a;   t1[0][1] = sin_a;  t1[0][2] = 0;
 t1[1][0] = -sin_a;   t1[1][1] = cos_a;  t1[1][2] = 0;
 t1[2][0] =  0;       t1[2][1] = 0;      t1[2][2] = 1;

 mvmul(t1,r,q1);
 
}
void eval_t2r2(rvec q1,real p2x,real p3x,matrix t2,matrix r2)
{
 real cos_a,sin_a,cos_o,sin_o;
 
 cos_a = (q1[0]-p2x)/p3x;
 sin_a = sqrt(1 - sqr(cos_a));
 if(sin_a < 0) 
 {
  sin_a = -sin_a;
 }
 cos_o = q1[1]/(sin_a*p3x);
 sin_o = q1[2]/(sin_a*p3x);

 t2[0][0] =  cos_a;   t2[0][1] = sin_a;  t2[0][2] = 0;
 t2[1][0] = -sin_a;   t2[1][1] = cos_a;  t2[1][2] = 0;
 t2[2][0] =  0;       t2[2][1] = 0;      t2[2][2] = 1;

 r2[0][0] =  1;    r2[0][1] = 0;       r2[0][2] = 0;
 r2[1][0] =  0;    r2[1][1] = cos_o;   r2[1][2] = sin_o;
 r2[2][0] =  0;    r2[2][1] = -sin_o;  r2[2][2] = cos_o;
 
}
real eval_gw(rvec u,matrix t0inv,matrix r1inv,matrix t1inv,matrix r2inv,matrix t2inv,matrix r3inv,real *cos_a3,real *sin_a3)
{
 rvec m1,m2,m3,m4,m5,m6;
 mvmul(t0inv,u,m1);
 mvmul(r1inv,m1,m2);
 mvmul(t1inv,m2,m3);
 mvmul(r2inv,m3,m4);
 mvmul(t2inv,m4,m5);
 mvmul(r3inv,m5,m6);

 *cos_a3 = m6[0];
 *sin_a3 = m6[1];

 return m6[2];
}
void eval_x(rvec *x,rvec *xprime,matrix basis,matrix basis_inv,real cos_a3,real sin_a3,rvec u,rvec v,matrix t0,matrix t0inv,matrix r3inv,matrix t2inv,matrix r2inv,matrix t1inv,matrix r1inv,int ah,int ai,int aj,int ak,int aa,int bb,int cc,real p0x,real p1x,real p2x,real p3x)
{
 matrix r2,t2,t1,r1,r3,r4,t3,t3inv;
 int i,j;
 rvec x4,x4b,x0,x1,x1b,x2,x2b,x3,x3b,x0b;
 rvec x21,x11,x23,x23b,x13,x33,x33b,teste1,teste2,x01,x03,x03b,x01b;
 rvec dak,dkc,dac;
 rvec xkk;
 rvec v1,v2,v3,v4,v5,v6,v7,u1,u2,u3;
 matrix basis3,basis_inv3;
 real kc;

 
 for(i=0;i<DIM;i++)
 {
  for(j=0;j<DIM;j++)
  {
   t1[j][i] = t1inv[i][j];
   r1[j][i] = r1inv[i][j];
   r2[j][i] = r2inv[i][j];
   t2[j][i] = t2inv[i][j];
   r3[j][i] = r3inv[i][j];
  }
 }


 t3[0][0] =  cos_a3;   t3[0][1] = -sin_a3;  t3[0][2] = 0;
 t3[1][0] =  sin_a3;   t3[1][1] =  cos_a3;  t3[1][2] = 0;
 t3[2][0] =  0;        t3[2][1] =  0;       t3[2][2] = 1;

 t3inv[0][0] =   cos_a3;   t3inv[0][1] =  sin_a3;  t3inv[0][2] = 0;
 t3inv[1][0] =  -sin_a3;   t3inv[1][1] =  cos_a3;  t3inv[1][2] = 0;
 t3inv[2][0] =   0;        t3inv[2][1] =  0;       t3inv[2][2] = 1;

 mvmul(t0inv,v,v1);
 mvmul(r1inv,v1,v2);
 mvmul(t1inv,v2,v3);
 mvmul(r2inv,v3,v4);
 mvmul(t2inv,v4,v5);
 mvmul(r3inv,v5,v6);
 mvmul(t3inv,v6,v7);

 r4[0][0] =  1;    r4[0][1] = 0;       r4[0][2] =  0;
 r4[1][0] =  0;    r4[1][1] = v7[1];   r4[1][2] = -v7[2];
 r4[2][0] =  0;    r4[2][1] = v7[2];   r4[2][2] =  v7[1];


 /* Find out xak in coordinate system 0*/
 x21[0]=p2x;
 x21[1]=x21[2]=0;

 mvmul(t1,x21,x11);
 x11[0] += p1x;
 mvmul(r1,x11,x01);
 mvmul(t0,x01,x01b);
 x01b[0] += p0x;

 /* turn this to the original coordinate system */
 mvmul(basis,x01b,xprime[ak]);
 rvec_add(xprime[ak],xprime[ah],xprime[ak]);
 //rvec_add(xai,xak,xak);

 rvec_sub(x[cc],x[ak],v1);
 rvec_sub(x[aa],x[ak],v2);
 rvec_sub(x[aa],x[bb],v3);
 cprod(v1,v2,v4);
 cprod(v2,v3,v5);
 unitv(v5,v5);
 v6[0] = iprod(v1,v2)/norm(v2);
 v6[1] = iprod(v4,v5)/norm(v2);
 if(iprod(v1,v5) >= 0)
 {
  v6[2] = sqrt(sqr(norm(v1)) - sqr(v6[1]) - sqr(v6[0]));
 }
 else
 {
  v6[2] = -sqrt(sqr(norm(v1)) - sqr(v6[1]) - sqr(v6[0]));
 }

 rvec_sub(x[aa],xprime[ak],u1);
 rvec_sub(x[bb],x[aa],v2);
 cprod(v2,u1,u3);
 cprod(u1,u3,u2);
 unitv(u3,u3);
 unitv(u2,u2);
 unitv(u1,u1);
 mk_basis(basis3,basis_inv3,u1,u2,u3);
 mvmul(basis3,v6,xprime[cc]);
 rvec_add(xprime[cc],xprime[ak],xprime[cc]);
/* rvec_sub(xprime[cc],xprime[ak],v1);
 rvec_sub(xprime[aa],xprime[ak],v2);
 rvec_sub(xprime[aa],xprime[bb],v3);
 cprod(v1,v2,v4);
 cprod(v2,v3,v5);
 unitv(v5,v5);
 rvec_sub(x[cc],x[ak],v1);
 mvmul(basis_inv3,v1,v2);

 /*
 x4[0] = iprod(v1,v2)/(norm(v2));
 x4[1] = -sqrt(sqr(norm(v1)) - sqr(x4[0]));
 x4[2] = 0;*/

 /*rvec_sub(x[aa],xprime[ak],u1);
 rvec_sub(x[bb],x[aa],v2);
 cprod(v2,u1,u3);
 cprod(u1,u3,u2);
 unitv(u3,u3);
 unitv(u2,u2);
 unitv(u1,u1);
 mk_basis(basis3,basis_inv3,u1,u2,u3);
 mvmul(basis_inv3,v1,x3);
 mvmul(r3,x3,x3b);
 mvmul(t2,x3b,x2);
 x2[0] += p2x;
 mvmul(r2,x2,x2b);
 mvmul(t1,x2b,x1);
 x1[0] += p1x;
 mvmul(r1,x1,x1b);
 mvmul(t0,x1b,x0);
 x0[0] += p0x;
 mvmul(basis,x0,xprime[cc]);
 rvec_add(xprime[cc],xprime[ah],xprime[cc]);
 /*mvmul(basis,x0,v1);
 rvec_add(v1,xprime[ah],v1);
 /*rvec_sub(v1,xprime[ak],v2);
 rvec_sub(x[aa],x[ak],v3);

 /* Find out xcc in coordinate system 0*/
 /*rvec_sub(x[ak],x[cc],v1);
 rvec_sub(x[aa],x[ak],v2);
 x3[0] = iprod(v1,v2)/norm(v2);
 x3[1] = sqrt(sqr(norm(v1)) - sqr(x3[0]));
 x3[2] = 0;

 mvmul(t2,x3,x2);
 x2[0] += p2x;
 mvmul(r2,x2,x2b);
 mvmul(t1,x2b,x1);
 x1[0] += p1x;
 mvmul(r1,x1,x0);
 mvmul(t0,x0,x0b);
 x0b[0] += p0x;

 mvmul(basis,x0b,xprime[cc]);
 rvec_add(xprime[cc],xprime[ah],xprime[cc]);
 //rvec_add(xai,xcc,xcc);

 /*x33[0]=p3x;
 x33[1]=x33[2]=0;
 mvmul(r3,x33,x33b);
 mvmul(t2,x33b,x23);
 x23[0] += p2x;
 mvmul(r2,x23,x23b);
 mvmul(t1,x23b,x13);
 x13[0] += p1x;
 mvmul(r1,x13,x03);
 mvmul(t0,x03,x03b);
 x03b[0] += p0x;
 mvmul(basis,x03b,xkk);
 rvec_add(xprime[kk],xprime[ah],xprime[kk]);
 //rvec_add(xai,xkk,xkk);*/

 /*rvec_sub(xak,xkk,teste1);
 rvec_sub(xcc,xak,teste2);*/
}

bool chain_closure(rvec *x,rvec *xprime,int ah,int ai,int aj,int ak,int aa,int bb,int cc,int dd)
{
 rvec q0,s;
 rvec xij,xka,xha,xab;
 rvec sq0,q1;
 rvec u1,u2,u3,r,u,v;
 rvec p0,p1,p2,p3;
 real alpha1,omega1,p2x,p3x;
 matrix basis,basis_inv;
 matrix t0,t0inv,r1inv,t1inv,t2inv,r2inv,r3inv;
 real gw1,gw2,gwm,o1,o2,om;
 rvec teste1,teste2;
 real cos_a0,sin_a0,cos_a3,sin_a3,dij,dhi;
 real delta_a1,delta_o1;
 rvec r_ij,r_kj,xcm,delta_phi;
 real omega1_0,alpha1_0,omega3;
 rvec v1,v2,v3,v4,v5;
 real f1,f2;
 real djk,dka,dac,dkc;
 int i1,i2,i3;
 bool found_root;
 bool branch2;

     /* We need to store the dihedral ah-ai-aj-ak (omega1) */
     /* for the root search in the chain closure routine */
     omega1_0 = dih_angle(x[ah],x[ai],x[aj],x[ak],NULL,v1,v2,v3,v4,v5,&f1,&f2,&i1,&i2,&i3);
     //printf("omega1 %f\n",omega1_0*180/M_PI);
     /* and aj-ak-aa-bb will be fixed... */
     omega3 = dih_angle(x[aj],x[ak],x[aa],x[bb],NULL,v1,v2,v3,v4,v5,&f1,&f2,&i1,&i2,&i3);
     alpha1_0 = bond_angle(x[ai],x[aj],x[ak],NULL,v1,v2,&f1,&i1,&i2);

     rvec_sub(x[aj],x[ak],v);
     djk = norm(v);
     rvec_sub(x[aa],x[ak],v);
     dka = norm(v);
     rvec_sub(x[aa],x[cc],v);
     dac = norm(v);
     rvec_sub(x[cc],x[ak],v);
     dkc = norm(v);

 /* omega3 will be fixed, so we use the stored value to construct matrix R3 */
 r3inv[0][0] =  1;    r3inv[0][1] =  0;           r3inv[0][2] =  0;
 r3inv[1][0] =  0;    r3inv[1][1] =  cos(omega3); r3inv[1][2] =  sin(omega3); 
 r3inv[2][0] =  0;    r3inv[2][1] = -sin(omega3); r3inv[2][2] =  cos(omega3);


 rvec_sub(xprime[ai],xprime[ah],u1);
 p0[0] = norm(u1);
 p0[1]=p0[2]=0;

 rvec_sub(xprime[aj],xprime[ai],xij);
 p1[0] = norm(xij);
 p1[1]=p1[2]=0;

 p2[0] = djk;
 p2[1]=p2[2]=0;

 p3[0] = dka;
 p3[1]=p3[2]=0;

 cos_a0 = iprod(u1,xij)/(p0[0]*p1[0]);
 sin_a0 = sqrt(1 - sqr(cos_a0));

 t0inv[0][0] =  cos_a0;   t0inv[0][1] = sin_a0;  t0inv[0][2] = 0;
 t0inv[1][0] = -sin_a0;   t0inv[1][1] = cos_a0;  t0inv[1][2] = 0;
 t0inv[2][0] =  0;        t0inv[2][1] = 0;       t0inv[2][2] = 1;

 t0[0][0] =  cos_a0;   t0[0][1] =-sin_a0;  t0[0][2] = 0;
 t0[1][0] =  sin_a0;   t0[1][1] = cos_a0;  t0[1][2] = 0;
 t0[2][0] =  0;        t0[2][1] = 0;       t0[2][2] = 1;

 cprod(u1,xij,u3);
 unitv(u3,u3);
 cprod(u3,u1,u2);
 unitv(u2,u2);
 unitv(u1,u1);

 mk_basis(basis,basis_inv,u1,u2,u3);

 rvec_sub(xprime[aa],xprime[ah],xha);
 mvmul(basis_inv,xha,s);

 mvmul(t0,p1,q0);
 rvec_add(q0,p0,q0);
 //rvec_sub(xaj,xah,u1);
 //mvmul(basis_inv,u1,q0);
 
 rvec_sub(xprime[aa],xprime[aj],u1);
 mvmul(basis_inv,u1,u2);
 rvec_sub(s,q0,sq0);

 rvec_sub(xprime[bb],xprime[aa],xab);
 mvmul(basis_inv,xab,u);
 unitv(u,u);
 rvec_sub(xprime[dd],xprime[bb],u1);
 cprod(u,u1,u2);
 cprod(u2,u,v);
 unitv(v,v);



 /* Search for roots of G(omega1) = 0 */
 o1=omega1_0 - 20*M_PI/180; 
 o2 = omega1_0 + 20*M_PI/180;
 //o1=-180*M_PI/180; 
 //o2 =180*M_PI/180;
 om=o1;

 eval_r(o1,t0inv,r1inv,sq0,r);
 eval_t1invq1(r,t1inv,q1,p2[0],p3[0],FALSE);
 eval_t2r2(q1,p2[0],p3[0],t2inv,r2inv);
 gw1 = eval_gw(u,t0inv,r1inv,t1inv,r2inv,t2inv,r3inv,&cos_a3,&sin_a3);
 eval_r(o2,t0inv,r1inv,sq0,r);
 eval_t1invq1(r,t1inv,q1,p2[0],p3[0],FALSE);
 eval_t2r2(q1,p2[0],p3[0],t2inv,r2inv);
 gw2 = eval_gw(u,t0inv,r1inv,t1inv,r2inv,t2inv,r3inv,&cos_a3,&sin_a3);

 found_root = FALSE;
 branch2 = FALSE;

 do
 {
  /*if((gw2 > 0 && gw1 > 0) || (gw2 < 0 && gw1 < 0))
  {
   o2 = o1;
   continue;
  }*/
  eval_r(om,t0inv,r1inv,sq0,r);
  eval_t1invq1(r,t1inv,q1,p2[0],p3[0],branch2);
  eval_t2r2(q1,p2[0],p3[0],t2inv,r2inv);
  gwm = eval_gw(u,t0inv,r1inv,t1inv,r2inv,t2inv,r3inv,&cos_a3,&sin_a3);

  /*if((gwm > 0 && gw1 > 0) || (gwm < 0 && gw1 < 0))
  {
   o1 = om;
   gw1 = gwm;
  }
  if((gwm > 0 && gw2 > 0) || (gwm < 0 && gw2 < 0))
  {
   o2 = om;
   gw2 = gwm;
  }*/
  if(gwm >= 0)
  {
  // printf("gwm %f %f\n",gwm,om*180/M_PI);
  }
  if(gwm >= 0 && gwm < 0.01)
  {
   found_root = TRUE;
   break;
  }
  if(!branch2)
  {
   branch2 = TRUE;
  }
  else
  {
   branch2 = FALSE;
   om += 0.05*M_PI/180;
  }
 } while(!found_root && om <= o2);
 //} while(!found_root && (o2 - o1) > 0.1*M_PI/180);
 if(!found_root)
 {
  return FALSE;
 }
 else
 {
  //printf("\n\nfound %f %f %d\n\n",om*180/M_PI,gwm,branch2);
 }

 eval_x(x,xprime,basis,basis_inv,cos_a3,sin_a3,u,v,t0,t0inv,r3inv,t2inv,r2inv,t1inv,r1inv,ah,ai,aj,ak,aa,bb,cc,p0[0],p1[0],p2[0],p3[0]);

 return TRUE;

}
void do_cra(rvec *x,gmx_mc_move *mc_move,t_graph *graph,gmx_rng_t rng,int homenr)
{
  int    n,i,k,start,end;
  int    ah,ai,aj,ak,nr,*list_r,jj,ii,aa,bb,kk,cc,dd;
  int    dihedral_nr = 7;
  int    angle_nr = 10;
  int    coord_nr;
  int    *dihedral_i,*dihedral_j,*dihedral_k,*angle_i,*angle_j,*angle_k;
  real   delta = 0.001;
  rvec   r_ij,r_kj,r_aaj,r1,r2,u1,u2,u3;
  rvec  *xprime,xaa[2],dvec[17];
  real  **matrix_i,**matrix_j,**matrix_l,**matrix_lt,**matrix_ltinv;
  real *delta_psi,*delta_chi;
  real cos;
  vec4 xrot;
  rvec xcm;
  matrix basis,basis_inv;
  rvec delta_phi;
  real coef1=100,coef2=8,coef3=20.0;
  real bias1,bias2;
  rvec xab,xdelta;
  real dij,djk,dka,dac,dkc;
  real cos_a;
  real f1,f2;
  rvec v1,v2,v3,v4,v5;
  real alpha1,omega1,omega3,p1,p2,p3;
  int i1,i2,i3;

  snew(list_r,homenr);
  snew(xprime,homenr);
  snew(dihedral_i,dihedral_nr);
  snew(dihedral_j,dihedral_nr);
  snew(dihedral_k,dihedral_nr);
  snew(angle_i,angle_nr);
  snew(angle_j,angle_nr);
  snew(angle_k,angle_nr);

  for(ii=0;ii<homenr;ii++)
  {
   copy_rvec(x[ii],xprime[ii]);
  }

  coord_nr = angle_nr + dihedral_nr;

  snew(matrix_i,coord_nr);
  snew(matrix_j,coord_nr);
  snew(matrix_l,coord_nr);
  snew(matrix_lt,coord_nr);
  snew(matrix_ltinv,coord_nr);
  snew(delta_psi,coord_nr);
  snew(delta_chi,coord_nr);
 
  for(ii=0;ii<coord_nr;ii++)
  {
   snew(matrix_i[ii],coord_nr);
   snew(matrix_j[ii],coord_nr);
   snew(matrix_l[ii],coord_nr);
   snew(matrix_lt[ii],coord_nr);
   snew(matrix_ltinv[ii],coord_nr);
  }

     jj = uniform_int(rng,(mc_move->group[MC_CRA].ilist)->nr/13);
     jj *= 13;

     /* PREROTATION */
     dihedral_i[0] = (mc_move->group[MC_CRA].ilist)->iatoms[jj];
     dihedral_j[0] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+1];
     dihedral_k[0] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+2];
     dihedral_i[1] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+1];
     dihedral_j[1] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+2];
     dihedral_k[1] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+3];
     dihedral_i[2] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+3];
     dihedral_j[2] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+4];
     dihedral_k[2] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+5];
     dihedral_i[3] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+4];
     dihedral_j[3] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+5];
     dihedral_k[3] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+6];
     dihedral_i[4] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+6];
     dihedral_j[4] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+7];
     dihedral_k[4] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+8];
     dihedral_i[5] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+7];
     dihedral_j[5] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+8];
     dihedral_k[5] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+9];
     dihedral_i[6] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+9];
     dihedral_j[6] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+10];
     dihedral_k[6] = (mc_move->group[MC_CRA].ilist)->iatoms[jj+11];

     ah = (mc_move->group[MC_CRA].ilist)->iatoms[jj+8];
     ai = (mc_move->group[MC_CRA].ilist)->iatoms[jj+9];
     aj = (mc_move->group[MC_CRA].ilist)->iatoms[jj+10];
     ak = (mc_move->group[MC_CRA].ilist)->iatoms[jj+11];
     aa = (mc_move->group[MC_CRA].ilist)->iatoms[jj+12];
     bb = (mc_move->group[MC_CRA].ilist)->iatoms[jj+13];
     cc = (mc_move->group[MC_CRA].ilist)->iatoms[jj+14];
     dd = (mc_move->group[MC_CRA].ilist)->iatoms[jj+15];

     /* We need to store the dihedral ah-ai-aj-ak (omega1) */
     /* for the root search in the chain closure routine */
     omega1 = dih_angle(x[ah],x[ai],x[aj],x[ak],NULL,v1,v2,v3,v4,v5,&f1,&f2,&i1,&i2,&i3);
     /* and aj-ak-aa-bb will be fixed... */
     omega3 = dih_angle(x[aj],x[ak],x[aa],x[bb],NULL,v1,v2,v3,v4,v5,&f1,&f2,&i1,&i2,&i3);
     omega3 = dih_angle(x[cc],x[ak],x[aa],x[bb],NULL,v1,v2,v3,v4,v5,&f1,&f2,&i1,&i2,&i3);
     alpha1 = bond_angle(x[ai],x[aj],x[ak],NULL,v1,v2,&f1,&i1,&i2);

     rvec_sub(x[aj],x[ai],xab);
     dij = norm(xab);
     rvec_sub(x[ak],x[aj],xab);
     djk = norm(xab);
     rvec_sub(x[ak],x[aa],xab);
     dka = norm(xab);
     rvec_sub(x[cc],x[ak],xdelta);
     dkc = norm(xdelta);

     cos_a = iprod(xab,xdelta)/(dkc*dka);
     

     copy_rvec(x[dihedral_k[6]],xab);
    
    

   for(ii=0;ii<dihedral_nr;ii++)
   {
     nr = 0;
     ai = dihedral_i[ii];
     aj = dihedral_j[ii];
     ak = dihedral_k[ii];


     rvec_sub(x[aj],x[ai],r_ij);
     rvec_sub(x[aj],x[ak],r_kj);
     cprod(r_ij,r_kj,r1);
     cprod(r_ij,r1,r2);

     unitv(r_ij,u1);
     unitv(r1,u2);
     unitv(r2,u3);
    
     mk_basis(basis,basis_inv,u1,u2,u3);


     clear_rvec(xcm);
     clear_rvec(delta_phi);

     for(kk=0;kk<2;kk++)
     {
      delta_phi[XX] = (!kk ? delta : -delta);
      clear_rvec(xaa[kk]);

      /*rvec_sub(x[26],x[29],r_ij);
      rvec_sub(x[31],x[29],r_kj);
      cos = acos(cos_angle(r_ij,r_kj));
      printf("hey %f\n",cos);*/
      mk_rot(x[aa],x[aj],r1,xcm,delta_phi,basis,basis_inv);
      //mk_rot(x,xprime,xcm,delta_phi,basis,basis_inv,aa,aj);
      copy_rvec(r1,xaa[kk]);
     }
     rvec_sub(xaa[0],xaa[1],dvec[ii]);
     svmul(1/(2*delta),dvec[ii],dvec[ii]);
  }
   mk_cra_angle_list(angle_i,angle_j,angle_k,mc_move,jj);
   for(ii=0;ii<angle_nr;ii++)
   {
     nr = 0;
     ai = angle_i[ii];
     aj = angle_j[ii];
     ak = angle_k[ii];


     rvec_sub(x[aj],x[ai],r_ij);
     rvec_sub(x[aj],x[ak],r_kj);
     cprod(r_ij,r_kj,r1);
     cprod(r_ij,r1,r2);

     unitv(r_ij,u2);
     unitv(r1,u1);
     unitv(r2,u3);
    
     mk_basis(basis,basis_inv,u1,u2,u3);


     clear_rvec(xcm);
     clear_rvec(delta_phi);

     for(kk=0;kk<2;kk++)
     {
      delta_phi[XX] = (!kk ? delta : -delta);
      clear_rvec(xaa[kk]);
      mk_rot(x[aa],x[aj],r1,xcm,delta_phi,basis,basis_inv);
      //mk_rot(x,xprime,xcm,delta_phi,basis,basis_inv,aa,aj);
      copy_rvec(r1,xaa[kk]);
     }
     rvec_sub(xaa[0],xaa[1],dvec[ii+dihedral_nr]);
     svmul(1/(2*delta),dvec[ii+dihedral_nr],dvec[ii+dihedral_nr]);
  }

  for(ii=0;ii<coord_nr;ii++)
  {
   for(kk=ii;kk<coord_nr;kk++)
   {
    matrix_i[ii][kk] = iprod(dvec[ii],dvec[kk]);
    matrix_i[kk][ii] = matrix_i[ii][kk];
   }
  }
  for(ii=0;ii<coord_nr;ii++)
  {
   for(kk=0;kk<coord_nr;kk++)
   {
    matrix_j[ii][kk] = coef1*coef2*matrix_i[ii][kk];
    if(ii==kk)
    {
     matrix_j[ii][kk] += coef1;
    }
   }
  }
  cholesky(matrix_j,matrix_l,matrix_lt,coef3,dihedral_nr,coord_nr);
  chi_to_psi(matrix_lt,rng,delta_chi,delta_psi,coord_nr);
  bias1= bias_prob(matrix_l,delta_chi,coord_nr);

   for(ii=0;ii<dihedral_nr-1;ii++)
   {
   if(ii>0)
    continue;
     ai = dihedral_i[ii];
     aj = dihedral_j[ii];
     ak = dihedral_k[ii];

     nr = 0;
     bond_rot(graph,ai,aj,list_r,&nr,aa);

     rvec_sub(xprime[aj],xprime[ai],r_ij);
     rvec_sub(xprime[aj],xprime[ak],r_kj);
     cprod(r_ij,r_kj,r1);
     cprod(r_ij,r1,r2);

     unitv(r_ij,u1);
     unitv(r1,u2);
     unitv(r2,u3);
    
     mk_basis(basis,basis_inv,u1,u2,u3);


     clear_rvec(xcm);
     clear_rvec(delta_phi);

     for(k=0;k<nr;k++) {
      delta_phi[XX] = delta_psi[ii];
      mk_rot(xprime[list_r[k]],xprime[aj],xprime[list_r[k]],xcm,delta_phi,basis,basis_inv);
     }
     rvec_sub(xprime[aj],xprime[ak],r_kj);
  }
   for(ii=0;ii<angle_nr-1;ii++)
   {
     continue;
     nr = 0;
     ai = angle_i[ii];
     aj = angle_j[ii];
     ak = angle_k[ii];
     bond_rot(graph,aj,ak,list_r,&nr,aa);
     list_r[nr++]=ak;


     rvec_sub(xprime[aj],xprime[ai],r_ij);
     rvec_sub(xprime[aj],xprime[ak],r_kj);
     cprod(r_ij,r_kj,r1);
     cprod(r_ij,r1,r2);

     unitv(r_ij,u2);
     unitv(r1,u1);
     unitv(r2,u3);
    
     mk_basis(basis,basis_inv,u1,u2,u3);


     clear_rvec(xcm);
     clear_rvec(delta_phi);

     for(k=0;k<nr;k++) 
     {
      delta_phi[XX] = delta_psi[dihedral_nr+ii];
      mk_rot(xprime[list_r[k]],xprime[aj],xprime[list_r[k]],xcm,delta_phi,basis,basis_inv);
     }
  }

     /*ai = dihedral_k[6];
     nr = 0;
     bond_rot(graph,ai,aa,list_r,&nr,-1);
     list_r[nr++]=aa;
     for(k=0;k<nr;k++) 
     {
      rvec_sub(x[list_r[k]],xab,xdelta);
      rvec_add(x[ai],xdelta,x[list_r[k]]);
     }*/

  /* Now we use the chain closure algorithm */

     ai = (mc_move->group[MC_CRA].ilist)->iatoms[jj+9];
     aj = (mc_move->group[MC_CRA].ilist)->iatoms[jj+10];
     ak = (mc_move->group[MC_CRA].ilist)->iatoms[jj+11];

  if(!chain_closure(x,xprime,ah,ai,aj,ak,aa,bb,cc,dd))
  {
   mc_move->bias = 0;
  }
     omega3 = dih_angle(xprime[cc],xprime[ak],xprime[aa],xprime[bb],NULL,v1,v2,v3,v4,v5,&f1,&f2,&i1,&i2,&i3);
     omega3 = dih_angle(xprime[aj],xprime[ak],xprime[aa],xprime[bb],NULL,v1,v2,v3,v4,v5,&f1,&f2,&i1,&i2,&i3);
     alpha1 = bond_angle(xprime[ai],xprime[aj],xprime[ak],NULL,v1,v2,&f1,&i1,&i2);

  rvec_sub(x[ak],x[cc],u1);

  for(ii=0;ii<homenr;ii++)
  {
   copy_rvec(xprime[ii],x[ii]);
  }
  rvec_sub(x[ak],x[cc],u1);
     omega1 = dih_angle(x[ah],x[ai],x[aj],x[ak],NULL,v1,v2,v3,v4,v5,&f1,&f2,&i1,&i2,&i3);
     //printf("omega1b %f\n",omega1*180/M_PI);
     
 /* Evaluate derivative for the inverse move*/
   for(ii=0;ii<dihedral_nr;ii++)
   {
     nr = 0;
     ai = dihedral_i[ii];
     aj = dihedral_j[ii];
     ak = dihedral_k[ii];


     rvec_sub(x[aj],x[ai],r_ij);
     rvec_sub(x[aj],x[ak],r_kj);
     cprod(r_ij,r_kj,r1);
     cprod(r_ij,r1,r2);

     unitv(r_ij,u1);
     unitv(r1,u2);
     unitv(r2,u3);
    
     mk_basis(basis,basis_inv,u1,u2,u3);


     clear_rvec(xcm);
     clear_rvec(delta_phi);

     for(kk=0;kk<2;kk++)
     {
      delta_phi[XX] = (!kk ? delta : -delta);
      clear_rvec(xaa[kk]);
      mk_rot(x[aa],x[aj],r1,xcm,delta_phi,basis,basis_inv);
      //mk_rot(x,xprime,xcm,delta_phi,basis,basis_inv,aa,aj);
      copy_rvec(r1,xaa[kk]);
     }
     rvec_sub(xaa[0],xaa[1],dvec[ii]);
     svmul(1/(2*delta),dvec[ii],dvec[ii]);
  }
   for(ii=0;ii<angle_nr;ii++)
   {
     nr = 0;
     ai = angle_i[ii];
     aj = angle_j[ii];
     ak = angle_k[ii];


     rvec_sub(x[aj],x[ai],r_ij);
     rvec_sub(x[aj],x[ak],r_kj);
     cprod(r_ij,r_kj,r1);
     cprod(r_ij,r1,r2);

     unitv(r_ij,u2);
     unitv(r1,u1);
     unitv(r2,u3);
    
     mk_basis(basis,basis_inv,u1,u2,u3);


     clear_rvec(xcm);
     clear_rvec(delta_phi);

     for(kk=0;kk<2;kk++)
     {
      delta_phi[XX] = (!kk ? delta : -delta);
      clear_rvec(xaa[kk]);
      mk_rot(x[aa],x[aj],r1,xcm,delta_phi,basis,basis_inv);
      //mk_rot(x,xprime,xcm,delta_phi,basis,basis_inv,aa,aj);
      copy_rvec(r1,xaa[kk]);
     }
     rvec_sub(xaa[0],xaa[1],dvec[ii+dihedral_nr]);
     svmul(1/(2*delta),dvec[ii+dihedral_nr],dvec[ii+dihedral_nr]);
  }

  for(ii=0;ii<coord_nr;ii++)
  {
   for(kk=ii;kk<coord_nr;kk++)
   {
    matrix_i[ii][kk] = iprod(dvec[ii],dvec[kk]);
    matrix_i[kk][ii] = matrix_i[ii][kk];
   }
  }
  for(ii=0;ii<coord_nr;ii++)
  {
   for(kk=0;kk<coord_nr;kk++)
   {
    matrix_j[ii][kk] = coef1*coef2*matrix_i[ii][kk];
    if(ii==kk)
    {
     matrix_j[ii][kk] += coef1;
    }
   }
  }

  cholesky(matrix_j,matrix_l,matrix_lt,coef3,dihedral_nr,coord_nr);
  psi_to_chi(matrix_lt,delta_chi,delta_psi,coord_nr);
  bias2=bias_prob(matrix_l,delta_chi,coord_nr);
  mc_move->bias = bias2/bias1;
  for(ii=0;ii<coord_nr;ii++)
  {
   sfree(matrix_i[ii]);
   sfree(matrix_j[ii]);
   sfree(matrix_l[ii]);
   sfree(matrix_lt[ii]);
   sfree(matrix_ltinv[ii]);
  }
  sfree(list_r);
  sfree(xprime);
  sfree(dihedral_i);
  sfree(dihedral_j);
  sfree(dihedral_k);
  sfree(angle_i);
  sfree(angle_j);
  sfree(angle_k);
  sfree(matrix_i);
  sfree(matrix_j);
  sfree(matrix_l);
  sfree(matrix_lt);
  sfree(matrix_ltinv);
  sfree(delta_psi);
  sfree(delta_chi);
}
void bend_angles(rvec *x,gmx_mc_move *mc_move,t_graph *graph)
{
  int    n,i,k,start,end;
  int    ai,aj,ak,al,nr_r,nr_l,nr,list_r[200],*list;
  rvec   r_ij,r_kj,r_lj,r1,r2,u1,u2,u3;
  vec4 xrot;
  rvec xcm;
  matrix basis,basis_inv;
  rvec delta_phi,delta;

     ai = mc_move->group[MC_ANGLES].ai;
     aj = mc_move->group[MC_ANGLES].aj;
     ak = mc_move->group[MC_ANGLES].ak;

     nr = 0;
     bond_rot(graph,aj,ak,list_r,&nr,-1);
     list=list_r;

     rvec_sub(x[aj],x[ai],r_ij);
     rvec_sub(x[aj],x[ak],r_kj);
     cprod(r_ij,r_kj,r1);
     cprod(r_ij,r1,r2);

     unitv(r_ij,u2);
     unitv(r1,u1);
     unitv(r2,u3);
    
     basis[XX][XX] = u1[XX]; basis[XX][YY] = u2[XX]; basis[XX][ZZ] = u3[XX]; 
     basis[YY][XX] = u1[YY]; basis[YY][YY] = u2[YY]; basis[YY][ZZ] = u3[YY]; 
     basis[ZZ][XX] = u1[ZZ]; basis[ZZ][YY] = u2[ZZ]; basis[ZZ][ZZ] = u3[ZZ]; 

     basis_inv[XX][XX] = basis[XX][XX]; basis_inv[XX][YY] = basis[YY][XX]; basis_inv[XX][ZZ] = basis[ZZ][XX]; 
     basis_inv[YY][XX] = basis[XX][YY]; basis_inv[YY][YY] = basis[YY][YY]; basis_inv[YY][ZZ] = basis[ZZ][YY]; 
     basis_inv[ZZ][XX] = basis[XX][ZZ]; basis_inv[ZZ][YY] = basis[YY][ZZ]; basis_inv[ZZ][ZZ] = basis[ZZ][ZZ]; 


     clear_rvec(xcm);
     clear_rvec(delta_phi);

     delta_phi[XX]=mc_move->group[MC_ANGLES].value/2;
     //delta_phi[XX]=-0.000003;

     list[nr++]=ak;

     for(k=0;k<nr;k++) {
      al=list[k];
      rvec_sub(x[al],x[aj],r_lj);
      mvmul(basis_inv,r_lj,r1);
      rand_rot_mc(r1,xrot,delta_phi,xcm);
      for(i=0;i<DIM;i++)
       r1[i]=xrot[i];
      mvmul(basis,r1,r2);
      rvec_add(x[aj],r2,x[al]);
     }

     nr = 0;
     bond_rot(graph,aj,ai,list_r,&nr,-1);
     list=list_r;

     clear_rvec(delta_phi);
     delta_phi[XX]=-mc_move->group[MC_ANGLES].value/2;
     //delta_phi[XX]=0.000003;

     list[nr++]=ai;

     for(k=0;k<nr;k++) {
      al=list[k];
      rvec_sub(x[al],x[aj],r_lj);
      mvmul(basis_inv,r_lj,r1);
      rand_rot_mc(r1,xrot,delta_phi,xcm);
      for(i=0;i<DIM;i++)
       r1[i]=xrot[i];
      mvmul(basis,r1,r2);
      rvec_add(x[aj],r2,x[al]);
     }
}
void set_mcmove(gmx_mc_movegroup *group,gmx_rng_t rng,real fac,int delta,int start,int i)
{
 int a;
 group->value = fac;
 group->ai = start + (group->ilist)->iatoms[delta*i];
 group->aj = start + (group->ilist)->iatoms[delta*i+1];
 group->bmove=TRUE;

 if(delta == 3) {
  group->ak = start + (group->ilist)->iatoms[delta*i+2];
  if(uniform_int(rng,2))
  {
  /* a = group->ai;
   group->ai = group->ak;
   group->ak = a;*/
  }
 }
 else 
 {
  if(uniform_int(rng,2))
  {
  /* a = group->ai;
   group->ai = group->aj;
   group->aj = a;*/
  }
 }
}
static void do_update_mc(rvec *x,real *massA,gmx_mc_move *mc_move,t_graph *graph,gmx_rng_t rng,int homenr)
{
  int    n,i,k,start,end;
  int    ai,aj,ak;
  rvec   r_ij,r_kj,r1,r2,u1,u2,u3;
  bool   b_translate,b_rotate;
  vec4 xrot;
  rvec xcm;
  real mass;
  start = mc_move->start;
  end = mc_move->end;
 
  b_translate = (mc_move->mvgroup == MC_TRANSLATE);
  b_rotate = (mc_move->mvgroup == MC_ROTATEX || mc_move->mvgroup == MC_ROTATEY || mc_move->mvgroup == MC_ROTATEZ);
   if(b_rotate) 
   {
    clear_rvec(xcm);
    mass=0;
    for(k=start;k<end;k++) {
     svmul(massA[k],x[k],r1);
     rvec_add(r1,xcm,xcm);
     mass += massA[k];
    } 
    svmul(1.0/mass,xcm,xcm);
   }
    for(n=start;n<end;n++) {
     if(b_rotate) 
     { 
      rand_rot_mc(x[n],xrot,mc_move->delta_phi,xcm);
      for(k=0;k<DIM;k++)
      {
       x[n][k]=xrot[k];
      }
     }
     if(b_translate)
     {
      rvec_add(x[n],mc_move->delta_x,x[n]);
     }
    }
  
    /* INTERNAL COORDINATES */
    if(mc_move->mvgroup == MC_DIHEDRALS) 
    {
     rotate_dihedral(x,mc_move,graph);
    }
    /* STRETCHING BONDS */
    if(mc_move->mvgroup == MC_BONDS) 
    {
     stretch_bonds(x,mc_move,graph);
    }
    /* BENDING ANGLES */
    if(mc_move->mvgroup == MC_ANGLES) 
    {
     bend_angles(x,mc_move,graph);
    }
    /* CRA */
    if(mc_move->mvgroup == MC_CRA) 
    {
     do_cra(x,mc_move,graph,rng,homenr);
    }
   
  //sfree(list_r);
  //sfree(list_l);
}
static void do_update_md(int start,int homenr,double dt,
                         t_grp_tcstat *tcstat,t_grp_acc *gstat,real nh_xi[],
                         rvec accel[],ivec nFreeze[],real invmass[],
                         unsigned short ptype[],unsigned short cFREEZE[],
                         unsigned short cACC[],unsigned short cTC[],
                         rvec x[],rvec xprime[],rvec v[],
                         rvec f[],matrix M,
                         bool bNH,bool bPR)
{
  double imass,w_dt;
  int    gf=0,ga=0,gt=0;
  rvec   vrel;
  real   vn,vv,va,vb,vnrel;
  real   lg,xi=0,u;
  int    n,d;

  if (bNH || bPR) {
    /* Update with coupling to extended ensembles, used for
     * Nose-Hoover and Parrinello-Rahman coupling
     * Nose-Hoover uses the reversible leap-frog integrator from
     * Holian et al. Phys Rev E 52(3) : 2338, 1995
     */
    for(n=start; n<start+homenr; n++) {
      imass = invmass[n];
      if (cFREEZE)
	gf   = cFREEZE[n];
      if (cACC)
	ga   = cACC[n];
      if (cTC)
	gt   = cTC[n];
      lg   = tcstat[gt].lambda;
      if (bNH)
          xi = nh_xi[gt];

      rvec_sub(v[n],gstat[ga].u,vrel);

      for(d=0; d<DIM; d++) {
        if((ptype[n] != eptVSite) && (ptype[n] != eptShell) && !nFreeze[gf][d]) {
            vnrel = (lg*vrel[d] + dt*(imass*f[n][d] - 0.5*xi*vrel[d]
				    - iprod(M[d],vrel)))/(1 + 0.5*xi*dt);  
          /* do not scale the mean velocities u */
          vn             = gstat[ga].u[d] + accel[ga][d]*dt + vnrel; 
          v[n][d]        = vn;
          xprime[n][d]   = x[n][d]+vn*dt;
        } else {
	  v[n][d]        = 0.0;
          xprime[n][d]   = x[n][d];
	}
      }
    }

  } else {
    /* Classic version of update, used with berendsen coupling */
    for(n=start; n<start+homenr; n++) {
      w_dt = invmass[n]*dt;
      if (cFREEZE)
	gf   = cFREEZE[n];
      if (cACC)
	ga   = cACC[n];
      if (cTC)
	gt   = cTC[n];
      lg   = tcstat[gt].lambda;

      for(d=0; d<DIM; d++) {
        vn             = v[n][d];

        if((ptype[n] != eptVSite) && (ptype[n] != eptShell) && !nFreeze[gf][d]) {
          vv             = lg*vn + f[n][d]*w_dt;

          /* do not scale the mean velocities u */
          u              = gstat[ga].u[d];
          va             = vv + accel[ga][d]*dt;
          vb             = va + (1.0-lg)*u;
          v[n][d]        = vb;
          xprime[n][d]   = x[n][d]+vb*dt;
        } else {
          v[n][d]        = 0.0;
          xprime[n][d]   = x[n][d];
        }
      }
    }
  }
}

void bond_rot(t_graph *graph,int ai,int aj,int *list,int *nr,int afix)
{
  int i,k;
  for(i=0;i<graph->nedge[aj];i++)
  {
   if(graph->edge[aj][i] != ai) 
   {
    for(k=0;k < (*nr);k++) 
    {
     if(graph->edge[aj][i] == list[k])
      break;
    }
    if(k == *nr)
    { 
     if(afix == -1 ||graph->edge[aj][i] != afix)
     { 
      list[(*nr)++] = graph->edge[aj][i];
      bond_rot(graph,aj,graph->edge[aj][i],list,nr,afix);
     }
    }
   }
  }
 return;
}
static void do_update_visc(int start,int homenr,double dt,
                           t_grp_tcstat *tcstat,real invmass[],real nh_xi[],
                           unsigned short ptype[],unsigned short cTC[],
                           rvec x[],rvec xprime[],rvec v[],
                           rvec f[],matrix M,matrix box,real
                           cos_accel,real vcos,
                           bool bNH,bool bPR)
{
  double imass,w_dt;
  int    gt=0;
  real   vn,vc;
  real   lg,xi=0,vv;
  real   fac,cosz;
  rvec   vrel;
  int    n,d;

  fac = 2*M_PI/(box[ZZ][ZZ]);

  if (bNH || bPR) {
    /* Update with coupling to extended ensembles, used for
     * Nose-Hoover and Parrinello-Rahman coupling
     */
    for(n=start; n<start+homenr; n++) {
      imass = invmass[n];
      if (cTC)
	gt   = cTC[n];
      lg   = tcstat[gt].lambda;
      cosz = cos(fac*x[n][ZZ]);

      copy_rvec(v[n],vrel);

      vc            = cosz*vcos;
      vrel[XX]     -= vc;
      if (bNH)
          xi        = nh_xi[gt];

      for(d=0; d<DIM; d++) {
        vn             = v[n][d];

        if((ptype[n] != eptVSite) && (ptype[n] != eptShell)) {
            vn  = (lg*vrel[d] + dt*(imass*f[n][d] - 0.5*xi*vrel[d]
				    - iprod(M[d],vrel)))/(1 + 0.5*xi*dt);
          if(d == XX)
            vn += vc + dt*cosz*cos_accel;

          v[n][d]        = vn;
          xprime[n][d]   = x[n][d]+vn*dt;
        } else
          xprime[n][d]   = x[n][d];
      }
    }

  } else {
    /* Classic version of update, used with berendsen coupling */
    for(n=start; n<start+homenr; n++) {
      w_dt = invmass[n]*dt;
      if (cTC)
        gt   = cTC[n];
      lg   = tcstat[gt].lambda;
      cosz = cos(fac*x[n][ZZ]);

      for(d=0; d<DIM; d++) {
        vn             = v[n][d];

        if((ptype[n] != eptVSite) && (ptype[n] != eptShell)) {
          if(d == XX) {
            vc           = cosz*vcos;
            /* Do not scale the cosine velocity profile */
            vv           = vc + lg*(vn - vc + f[n][d]*w_dt);
            /* Add the cosine accelaration profile */
            vv          += dt*cosz*cos_accel;
          } else
            vv           = lg*vn + f[n][d]*w_dt;

          v[n][d]        = vv;
          xprime[n][d]   = x[n][d]+vv*dt;
        } else {
          v[n][d]        = 0.0;
          xprime[n][d]   = x[n][d];
        }
      }
    }
  }
}

static gmx_stochd_t *init_stochd(FILE *fplog,t_inputrec *ir)
{
    gmx_stochd_t *sd;
    gmx_sd_const_t *sdc;
    int  ngtc,n;
    real y;
    
  snew(sd,1);

  /* Initiate random number generator for langevin type dynamics,
   * for BD, SD or velocity rescaling temperature coupling.
   */
  sd->gaussrand = gmx_rng_init(ir->ld_seed);

  ngtc = ir->opts.ngtc;

  if (ir->eI == eiBD) {
    snew(sd->bd_rf,ngtc);
  } else if (EI_SD(ir->eI)) {
    snew(sd->sdc,ngtc);
    snew(sd->sdsig,ngtc);
    
    sdc = sd->sdc;
    for(n=0; n<ngtc; n++) {
      sdc[n].gdt = ir->delta_t/ir->opts.tau_t[n];
      sdc[n].eph = exp(sdc[n].gdt/2);
      sdc[n].emh = exp(-sdc[n].gdt/2);
      sdc[n].em  = exp(-sdc[n].gdt);
      if (sdc[n].gdt >= 0.05) {
	sdc[n].b = sdc[n].gdt*(sdc[n].eph*sdc[n].eph - 1) 
	  - 4*(sdc[n].eph - 1)*(sdc[n].eph - 1);
	sdc[n].c = sdc[n].gdt - 3 + 4*sdc[n].emh - sdc[n].em;
	sdc[n].d = 2 - sdc[n].eph - sdc[n].emh;
      } else {
	y = sdc[n].gdt/2;
	/* Seventh order expansions for small y */
	sdc[n].b = y*y*y*y*(1/3.0+y*(1/3.0+y*(17/90.0+y*7/9.0)));
	sdc[n].c = y*y*y*(2/3.0+y*(-1/2.0+y*(7/30.0+y*(-1/12.0+y*31/1260.0))));
	sdc[n].d = y*y*(-1+y*y*(-1/12.0-y*y/360.0));
      }
      if(debug)
	fprintf(debug,"SD const tc-grp %d: b %g  c %g  d %g\n",
		n,sdc[n].b,sdc[n].c,sdc[n].d);
    }
  }

  return sd;
}

void get_stochd_state(gmx_update_t upd,t_state *state)
{
  gmx_rng_get_state(upd->sd->gaussrand,state->ld_rng,state->ld_rngi);
}

void set_stochd_state(gmx_update_t upd,t_state *state)
{
  gmx_rng_set_state(upd->sd->gaussrand,state->ld_rng,state->ld_rngi[0]);
}

gmx_update_t init_update(FILE *fplog,t_inputrec *ir)
{
    t_gmx_update *upd;
    
    snew(upd,1);
    
    if (ir->eI == eiBD || EI_SD(ir->eI) || ir->etc == etcVRESCALE)
    {
        upd->sd = init_stochd(fplog,ir);
    }

    upd->xp = NULL;
    upd->xp_nalloc = 0;

    return upd;
}

static void do_update_sd1(gmx_stochd_t *sd,
                          int start,int homenr,double dt,
                          rvec accel[],ivec nFreeze[],
                          real invmass[],unsigned short ptype[],
                          unsigned short cFREEZE[],unsigned short cACC[],
                          unsigned short cTC[],
                          rvec x[],rvec xprime[],rvec v[],rvec f[],
                          rvec sd_X[],
                          int ngtc,real tau_t[],real ref_t[])
{
  gmx_sd_const_t *sdc;
  gmx_sd_sigma_t *sig;
  gmx_rng_t gaussrand;
  real   kT;
  int    gf=0,ga=0,gt=0;
  real   ism,sd_V;
  int    n,d;

  sdc = sd->sdc;
  sig = sd->sdsig;
  if (homenr > sd->sd_V_nalloc) {
    sd->sd_V_nalloc = over_alloc_dd(homenr);
    srenew(sd->sd_V,sd->sd_V_nalloc);
  }
  gaussrand = sd->gaussrand;
  
  for(n=0; n<ngtc; n++) {
    kT = BOLTZ*ref_t[n];
    /* The mass is encounted for later, since this differs per atom */
    sig[n].V  = sqrt(2*kT*(1 - sdc[n].em));
  }

  for(n=start; n<start+homenr; n++) {
    ism = sqrt(invmass[n]);
    if (cFREEZE)
      gf  = cFREEZE[n];
    if (cACC)
      ga  = cACC[n];
    if (cTC)
      gt  = cTC[n];

    for(d=0; d<DIM; d++) {
      if((ptype[n] != eptVSite) && (ptype[n] != eptShell) && !nFreeze[gf][d]) {
	sd_V = ism*sig[gt].V*gmx_rng_gaussian_table(gaussrand);
	
	v[n][d] = v[n][d]*sdc[gt].em 
	  + (invmass[n]*f[n][d] + accel[ga][d])*tau_t[gt]*(1 - sdc[gt].em)
	  + sd_V;

	xprime[n][d] = x[n][d] + v[n][d]*dt;
      } else {
	v[n][d]      = 0.0;
	xprime[n][d] = x[n][d];
      }
    }
  }
}

static void do_update_sd2(gmx_stochd_t *sd,bool bInitStep,
                          int start,int homenr,
                          rvec accel[],ivec nFreeze[],
                          real invmass[],unsigned short ptype[],
                          unsigned short cFREEZE[],unsigned short cACC[],
                          unsigned short cTC[],
                          rvec x[],rvec xprime[],rvec v[],rvec f[],
                          rvec sd_X[],
                          int ngtc,real tau_t[],real ref_t[],
                          bool bFirstHalf)
{
  gmx_sd_const_t *sdc;
  gmx_sd_sigma_t *sig;
  /* The random part of the velocity update, generated in the first
   * half of the update, needs to be remembered for the second half.
   */
  rvec *sd_V;
  gmx_rng_t gaussrand;
  real   kT;
  int    gf=0,ga=0,gt=0;
  real   vn=0,Vmh,Xmh;
  real   ism;
  int    n,d;

  sdc = sd->sdc;
  sig = sd->sdsig;
  if (homenr > sd->sd_V_nalloc) {
    sd->sd_V_nalloc = over_alloc_dd(homenr);
    srenew(sd->sd_V,sd->sd_V_nalloc);
  }
  sd_V = sd->sd_V;
  gaussrand = sd->gaussrand;

  if(bFirstHalf) {
    for(n=0; n<ngtc; n++) {
      kT = BOLTZ*ref_t[n];
      /* The mass is encounted for later, since this differs per atom */
      sig[n].V  = sqrt(kT*(1-sdc[n].em));
      sig[n].X  = sqrt(kT*sqr(tau_t[n])*sdc[n].c);
      sig[n].Yv = sqrt(kT*sdc[n].b/sdc[n].c);
      sig[n].Yx = sqrt(kT*sqr(tau_t[n])*sdc[n].b/(1-sdc[n].em));
    }
  }

  for(n=start; n<start+homenr; n++) {
    ism = sqrt(invmass[n]);
    if (cFREEZE)
      gf  = cFREEZE[n];
    if (cACC)
      ga  = cACC[n];
    if (cTC)
      gt  = cTC[n];

    for(d=0; d<DIM; d++) {
      if(bFirstHalf) {
        vn             = v[n][d];
      }
      if((ptype[n] != eptVSite) && (ptype[n] != eptShell) && !nFreeze[gf][d]) {
        if (bFirstHalf) {

          if (bInitStep)
            sd_X[n][d] = ism*sig[gt].X*gmx_rng_gaussian_table(gaussrand);

          Vmh = sd_X[n][d]*sdc[gt].d/(tau_t[gt]*sdc[gt].c) 
                + ism*sig[gt].Yv*gmx_rng_gaussian_table(gaussrand);
          sd_V[n-start][d] = ism*sig[gt].V*gmx_rng_gaussian_table(gaussrand);

          v[n][d] = vn*sdc[gt].em 
                    + (invmass[n]*f[n][d] + accel[ga][d])*tau_t[gt]*(1 - sdc[gt].em)
                    + sd_V[n-start][d] - sdc[gt].em*Vmh;

          xprime[n][d] = x[n][d] + v[n][d]*tau_t[gt]*(sdc[gt].eph - sdc[gt].emh); 

        } else {

          /* Correct the velocities for the constraints.
	   * This operation introduces some inaccuracy,
	   * since the velocity is determined from differences in coordinates.
	   */
          v[n][d] = 
          (xprime[n][d] - x[n][d])/(tau_t[gt]*(sdc[gt].eph - sdc[gt].emh));  

          Xmh = sd_V[n-start][d]*tau_t[gt]*sdc[gt].d/(sdc[gt].em-1) 
                + ism*sig[gt].Yx*gmx_rng_gaussian_table(gaussrand);
          sd_X[n][d] = ism*sig[gt].X*gmx_rng_gaussian_table(gaussrand);

          xprime[n][d] += sd_X[n][d] - Xmh;

        }
      } else {
        if(bFirstHalf) {
          v[n][d]        = 0.0;
          xprime[n][d]   = x[n][d];
        }
      }
    }
  }
}

static void do_update_bd(int start,int homenr,double dt,
                         ivec nFreeze[],
                         real invmass[],unsigned short ptype[],
                         unsigned short cFREEZE[],unsigned short cTC[],
                         rvec x[],rvec xprime[],rvec v[],
                         rvec f[],real friction_coefficient,
                         int ngtc,real tau_t[],real ref_t[],
			 real *rf,gmx_rng_t gaussrand)
{
  int    gf=0,gt=0;
  real   vn;
  real   invfr=0;
  int    n,d;

  if (friction_coefficient != 0) {
    invfr = 1.0/friction_coefficient;
    for(n=0; n<ngtc; n++)
      rf[n] = sqrt(2.0*BOLTZ*ref_t[n]/(friction_coefficient*dt));
  } else
    for(n=0; n<ngtc; n++)
      rf[n] = sqrt(2.0*BOLTZ*ref_t[n]);

  for(n=start; (n<start+homenr); n++) {
    if (cFREEZE)
      gf = cFREEZE[n];
    if (cTC)
      gt = cTC[n];
    for(d=0; (d<DIM); d++) {
      if((ptype[n]!=eptVSite) && (ptype[n]!=eptShell) && !nFreeze[gf][d]) {
        if (friction_coefficient != 0)
          vn = invfr*f[n][d] + rf[gt]*gmx_rng_gaussian_table(gaussrand);
        else
          /* NOTE: invmass = 1/(mass*friction_constant*dt) */
          vn = invmass[n]*f[n][d]*dt 
	    + sqrt(invmass[n])*rf[gt]*gmx_rng_gaussian_table(gaussrand);

        v[n][d]      = vn;
        xprime[n][d] = x[n][d]+vn*dt;
      } else {
        v[n][d]      = 0.0;
        xprime[n][d] = x[n][d];
      }
    }
  }
}

static void dump_it_all(FILE *fp,const char *title,
                        int natoms,rvec x[],rvec xp[],rvec v[],rvec f[])
{
#ifdef DEBUG
  if (fp) {
    fprintf(fp,"%s\n",title);
    pr_rvecs(fp,0,"x",x,natoms);
    pr_rvecs(fp,0,"xp",xp,natoms);
    pr_rvecs(fp,0,"v",v,natoms);
    pr_rvecs(fp,0,"f",f,natoms);
  }
#endif
}

static void calc_ke_part_normal(rvec v[],t_grpopts *opts,t_mdatoms *md,
                                gmx_ekindata_t *ekind,t_nrnb *nrnb)
{
  int          start=md->start,homenr=md->homenr;
  int          g,d,n,ga=0,gt=0;
  rvec         v_corrt;
  real         hm;
  t_grp_tcstat *tcstat=ekind->tcstat;
  t_grp_acc    *grpstat=ekind->grpstat;
  real         dekindl;

  /* group velocities are calculated in update_ekindata and
   * accumulated in acumulate_groups.
   * Now the partial global and groups ekin.
   */
  for(g=0; (g<opts->ngtc); g++) {
    copy_mat(ekind->tcstat[g].ekinh,ekind->tcstat[g].ekinh_old);
    clear_mat(ekind->tcstat[g].ekinh);
  }
  ekind->dekindl_old = ekind->dekindl;

  dekindl = 0;
  for(n=start; (n<start+homenr); n++) {
    if (md->cACC)
      ga = md->cACC[n];
    if (md->cTC)
      gt = md->cTC[n];
    hm   = 0.5*md->massT[n];

    for(d=0; (d<DIM); d++) {
      v_corrt[d] = v[n][d] - grpstat[ga].u[d];
    }
    for(d=0; (d<DIM); d++) {
      tcstat[gt].ekinh[XX][d]+=hm*v_corrt[XX]*v_corrt[d];
      tcstat[gt].ekinh[YY][d]+=hm*v_corrt[YY]*v_corrt[d];
      tcstat[gt].ekinh[ZZ][d]+=hm*v_corrt[ZZ]*v_corrt[d];
    }
    if (md->nMassPerturbed && md->bPerturbed[n])
      dekindl -= 0.5*(md->massB[n] - md->massA[n])*iprod(v_corrt,v_corrt);
  }
  ekind->dekindl = dekindl;

  inc_nrnb(nrnb,eNR_EKIN,homenr);
}

static void calc_ke_part_visc(matrix box,rvec x[],rvec v[],
                              t_grpopts *opts,t_mdatoms *md,
                              gmx_ekindata_t *ekind,
                              t_nrnb *nrnb)
{
  int          start=md->start,homenr=md->homenr;
  int          g,d,n,gt=0;
  rvec         v_corrt;
  real         hm;
  t_grp_tcstat *tcstat=ekind->tcstat;
  t_cos_acc    *cosacc=&(ekind->cosacc);
  real         dekindl;
  real         fac,cosz;
  double       mvcos;

  for(g=0; g<opts->ngtc; g++) {
    copy_mat(ekind->tcstat[g].ekinh,ekind->tcstat[g].ekinh_old);
    clear_mat(ekind->tcstat[g].ekinh);
  }
  ekind->dekindl_old = ekind->dekindl;

  fac = 2*M_PI/box[ZZ][ZZ];
  mvcos = 0;
  dekindl = 0;
  for(n=start; n<start+homenr; n++) {
    if (md->cTC)
      gt = md->cTC[n];
    hm   = 0.5*md->massT[n];

    /* Note that the times of x and v differ by half a step */
    cosz         = cos(fac*x[n][ZZ]);
    /* Calculate the amplitude of the new velocity profile */
    mvcos       += 2*cosz*md->massT[n]*v[n][XX];

    copy_rvec(v[n],v_corrt);
    /* Subtract the profile for the kinetic energy */
    v_corrt[XX] -= cosz*cosacc->vcos;
    for(d=0; (d<DIM); d++) {
      tcstat[gt].ekinh[XX][d]+=hm*v_corrt[XX]*v_corrt[d];
      tcstat[gt].ekinh[YY][d]+=hm*v_corrt[YY]*v_corrt[d];
      tcstat[gt].ekinh[ZZ][d]+=hm*v_corrt[ZZ]*v_corrt[d];
    }
    if(md->nPerturbed && md->bPerturbed[n])
      dekindl -= 0.5*(md->massB[n] - md->massA[n])*iprod(v_corrt,v_corrt);
  }
  ekind->dekindl = dekindl;
  cosacc->mvcos = mvcos;

  inc_nrnb(nrnb,eNR_EKIN,homenr);
}

void calc_ke_part(t_state *state,
                  t_grpopts *opts,t_mdatoms *md,
                  gmx_ekindata_t *ekind,
                  t_nrnb *nrnb)
{
    if (ekind->cosacc.cos_accel == 0)
    {
        calc_ke_part_normal(state->v,opts,md,ekind,nrnb);
    }
    else
    {
        calc_ke_part_visc(state->box,state->x,state->v,opts,md,ekind,nrnb);
    }
}

void init_ekinstate(ekinstate_t *ekinstate,t_inputrec *ir)
{
  ekinstate->ekinh_n = ir->opts.ngtc;
  snew(ekinstate->ekinh,ekinstate->ekinh_n);
  ekinstate->dekindl = 0;
  ekinstate->mvcos   = 0;
}

void
update_ekinstate(ekinstate_t *ekinstate,gmx_ekindata_t *ekind)
{
  int i;
  
  for(i=0;i<ekinstate->ekinh_n;i++) {
    copy_mat(ekind->tcstat[i].ekinh,ekinstate->ekinh[i]);
  }
  ekinstate->dekindl = ekind->dekindl;
  ekinstate->mvcos = ekind->cosacc.mvcos;
  
}

void
restore_ekinstate_from_state(t_commrec *cr,
			     gmx_ekindata_t *ekind,ekinstate_t *ekinstate)
{
  int i,n;

  if (MASTER(cr)) {
    for(i=0;i<ekinstate->ekinh_n;i++) {
      copy_mat(ekinstate->ekinh[i],ekind->tcstat[i].ekinh);
    }
    ekind->dekindl = ekinstate->dekindl;
    ekind->cosacc.mvcos = ekinstate->mvcos;
    n = ekinstate->ekinh_n;
  }
 
  if (PAR(cr)) {
    gmx_bcast(sizeof(n),&n,cr);
    for(i=0;i<n;i++) {
      gmx_bcast(DIM*DIM*sizeof(ekind->tcstat[i].ekinh[0][0]),
		ekind->tcstat[i].ekinh[0],cr);
    }
    gmx_bcast(sizeof(ekind->dekindl),&ekind->dekindl,cr);
    gmx_bcast(sizeof(ekind->cosacc.mvcos),&ekind->cosacc.mvcos,cr);
  }
}

void set_deform_reference_box(gmx_update_t upd,gmx_step_t step,matrix box)
{
    upd->deformref_step = step;
    copy_mat(box,upd->deformref_box);
}

static void deform(gmx_update_t upd,
                   int start,int homenr,rvec x[],matrix box,matrix *scale_tot,
                   const t_inputrec *ir,gmx_step_t step)
{
    matrix bnew,invbox,mu;
    real   elapsed_time;
    int    i,j;  
    
    elapsed_time = (step + 1 - upd->deformref_step)*ir->delta_t;
    copy_mat(box,bnew);
    for(i=0; i<DIM; i++)
    {
        for(j=0; j<DIM; j++)
        {
            if (ir->deform[i][j] != 0)
            {
                bnew[i][j] =
                    upd->deformref_box[i][j] + elapsed_time*ir->deform[i][j];
            }
        }
    }
    /* We correct the off-diagonal elements,
     * which can grow indefinitely during shearing,
     * so the shifts do not get messed up.
     */
    for(i=1; i<DIM; i++)
    {
        for(j=i-1; j>=0; j--)
        {
            while (bnew[i][j] - box[i][j] > 0.5*bnew[j][j])
            {
                rvec_dec(bnew[i],bnew[j]);
            }
            while (bnew[i][j] - box[i][j] < -0.5*bnew[j][j])
            {
                rvec_inc(bnew[i],bnew[j]);
            }
        }
    }
    m_inv_ur0(box,invbox);
    copy_mat(bnew,box);
    mmul_ur0(box,invbox,mu);
  
    for(i=start; i<start+homenr; i++)
    {
        x[i][XX] = mu[XX][XX]*x[i][XX]+mu[YY][XX]*x[i][YY]+mu[ZZ][XX]*x[i][ZZ];
        x[i][YY] = mu[YY][YY]*x[i][YY]+mu[ZZ][YY]*x[i][ZZ];
        x[i][ZZ] = mu[ZZ][ZZ]*x[i][ZZ];
    }
    if (*scale_tot)
    {
        /* The transposes of the scaling matrices are stored,
         * so we need to do matrix multiplication in the inverse order.
         */
        mmul_ur0(*scale_tot,mu,*scale_tot);
    }
}

static void combine_forces(int nstlist,
                           gmx_constr_t constr,
                           t_inputrec *ir,t_mdatoms *md,t_idef *idef,
                           t_commrec *cr,gmx_step_t step,t_state *state,
                           int start,int homenr,
                           rvec f[],rvec f_lr[],
                           t_nrnb *nrnb)
{
    int  i,d,nm1;

    /* f contains the short-range forces + the long range forces
     * which are stored separately in f_lr.
     */

    if (constr != NULL && !(ir->eConstrAlg == econtSHAKE && ir->epc == epcNO))
    {
        /* We need to constrain the LR forces separately,
         * because due to the different pre-factor for the SR and LR
         * forces in the update algorithm, we can not determine
         * the constraint force for the coordinate constraining.
         * Constrain only the additional LR part of the force.
         */
        constrain(NULL,FALSE,FALSE,constr,idef,ir,cr,step,0,md,
                  state->x,f_lr,f_lr,state->box,state->lambda,NULL,
                  NULL,NULL,nrnb,econqForce);
    }
    
    /* Add nstlist-1 times the LR force to the sum of both forces
     * and store the result in forces_lr.
     */
    nm1 = nstlist - 1;
    for(i=start; i<start+homenr; i++)
    {
        for(d=0; d<DIM; d++)
        {
            f_lr[i][d] = f[i][d] + nm1*f_lr[i][d];
        }
    }
}
void update(FILE         *fplog,
            gmx_step_t   step,
            real         *dvdlambda,    /* FEP stuff */
            t_inputrec   *inputrec,     /* input record and box stuff	*/
            t_mdatoms    *md,
            t_state      *state,
            t_graph      *graph,  
            rvec         *f,            /* forces on home particles */
            bool         bDoLR,
            rvec         *f_lr,
            t_fcdata     *fcd,
            t_idef       *idef,
            gmx_ekindata_t *ekind,
            matrix       *scale_tot,
            t_commrec    *cr,
            t_forcerec   *fr,
            t_nrnb       *nrnb,
            t_block      *mols,
            gmx_wallcycle_t wcycle,
            gmx_update_t upd,
            gmx_constr_t constr,
            bool         bCalcVir,
            tensor       vir_part,
            bool         bNEMD,
            bool         bInitStep,
            gmx_rng_t    rng)
{
    bool             bCouple,bNH,bPR,bLastStep,bLog=FALSE,bEner=FALSE;
    double           dt,eph;
    real             dt_1,dtc;
    int              start,homenr,i,n,m,g;
    matrix           pcoupl_mu,M;
    rvec             *force;
    tensor           vir_con;
    rvec             *xprime;
    real             vnew,vfrac;
    gmx_mc_move      *mc_move;
    rvec             v1;

   mc_move = state->mc_move;
    
    start  = md->start;
    homenr = md->homenr;
    
    if (state->nalloc > upd->xp_nalloc)
    {
        upd->xp_nalloc = state->nalloc;
        srenew(upd->xp,upd->xp_nalloc);
    }

    xprime = upd->xp;
    
    /* We need to update the NMR restraint history when time averaging is used */
    if (state->flags & (1<<estDISRE_RM3TAV)) {
        update_disres_history(fcd,&state->hist);
    }
    if (state->flags & (1<<estORIRE_DTAV)) {
        update_orires_history(fcd,&state->hist);
    }
    
    /* We should only couple after a step where energies were determined */
    bCouple = (inputrec->nstcalcenergy == 1 ||
               do_per_step(step+inputrec->nstcalcenergy-1,
                           inputrec->nstcalcenergy));
    dtc = inputrec->nstcalcenergy*inputrec->delta_t;
    
    bNH = (inputrec->etc == etcNOSEHOOVER);
    bPR = (inputrec->epc == epcPARRINELLORAHMAN);
    
    dt   = inputrec->delta_t;
    dt_1 = 1.0/dt;
    
    clear_mat(pcoupl_mu);
    for(i=0; i<DIM; i++)
    {
        pcoupl_mu[i][i] = 1.0;
    }
    clear_mat(M);

    if (bCouple)
    {
        switch (inputrec->etc)
        {
        case etcNO:
            break;
        case etcMC:
            break;
        case etcBERENDSEN:
            berendsen_tcoupl(&(inputrec->opts),ekind,dtc);
            break;
        case etcNOSEHOOVER:
            nosehoover_tcoupl(&(inputrec->opts),ekind,dtc,
                              state->nosehoover_xi,state->therm_integral);
            break;
        case etcVRESCALE:
            vrescale_tcoupl(&(inputrec->opts),ekind,dtc,
                            state->therm_integral,upd->sd->gaussrand);
        break;
        }

        
        if (inputrec->epc == epcBERENDSEN && !bInitStep)
        {
            berendsen_pcoupl(fplog,step,inputrec,dtc,
                             state->pres_prev,state->box,
                             pcoupl_mu);
        }
        if (inputrec->epc == epcPARRINELLORAHMAN)
        {
            parrinellorahman_pcoupl(fplog,step,inputrec,dtc,state->pres_prev,
                                    state->box,state->box_rel,state->boxv,
                                    M,pcoupl_mu,bInitStep);
        }
        if (inputrec->epc == epcMC && mc_move->update_box)
        {
            mc_pcoupl(fplog,step,inputrec,dtc,
                             state->pres_prev,state->box,
                             pcoupl_mu,mc_move,mols,state->x,md->massA);
        }
    }
    else
    {
        /* Set the T scaling lambda to 1 to have no scaling */
        for(i=0; (i<inputrec->opts.ngtc); i++)
        {
            ekind->tcstat[i].lambda = 1.0;
        }
    }

    if (bDoLR && inputrec->nstlist > 1)
    {
        /* Store the total force + nstlist-1 times the LR force
         * in forces_lr, so it can be used in a normal update algorithm
         * to produce twin time stepping.
         */
        combine_forces(inputrec->nstlist,constr,inputrec,md,idef,cr,step,state,
                       start,homenr,f,f_lr,nrnb);
        force = f_lr;
    }
    else
    {
        force = f;
    }

    /* Now do the actual update of velocities and positions */
    where();
    dump_it_all(fplog,"Before update",
                state->natoms,state->x,xprime,state->v,force);

  if (inputrec->eI == eiMD) {
    if (ekind->cosacc.cos_accel == 0) {
      /* use normal version of update */
      do_update_md(start,homenr,dt,
		   ekind->tcstat,ekind->grpstat,state->nosehoover_xi,
		   inputrec->opts.acc,inputrec->opts.nFreeze,md->invmass,md->ptype,
		   md->cFREEZE,md->cACC,md->cTC,
		   state->x,xprime,state->v,force,M,
		   bNH,bPR);
    } else {
      do_update_visc(start,homenr,dt,
		     ekind->tcstat,md->invmass,state->nosehoover_xi,
		     md->ptype,md->cTC,state->x,xprime,state->v,force,M,
		     state->box,ekind->cosacc.cos_accel,ekind->cosacc.vcos,
		     bNH,bPR);
    }
  } else if (inputrec->eI == eiSD1) {
    do_update_sd1(upd->sd,start,homenr,dt,
		  inputrec->opts.acc,inputrec->opts.nFreeze,
		  md->invmass,md->ptype,
		  md->cFREEZE,md->cACC,md->cTC,
		  state->x,xprime,state->v,force,state->sd_X,
		  inputrec->opts.ngtc,inputrec->opts.tau_t,inputrec->opts.ref_t);
  } else if (inputrec->eI == eiSD2) {
    /* The SD update is done in 2 parts, because an extra constraint step
     * is needed 
     */
    do_update_sd2(upd->sd,bInitStep,start,homenr,
		  inputrec->opts.acc,inputrec->opts.nFreeze,
		  md->invmass,md->ptype,
		  md->cFREEZE,md->cACC,md->cTC,
		  state->x,xprime,state->v,force,state->sd_X,
		  inputrec->opts.ngtc,inputrec->opts.tau_t,inputrec->opts.ref_t,
		  TRUE);
  } else if (inputrec->eI == eiBD) {
    do_update_bd(start,homenr,dt,
		 inputrec->opts.nFreeze,md->invmass,md->ptype,
		 md->cFREEZE,md->cTC,
		 state->x,xprime,state->v,force,
		 inputrec->bd_fric,
		 inputrec->opts.ngtc,inputrec->opts.tau_t,inputrec->opts.ref_t,
		 upd->sd->bd_rf,upd->sd->gaussrand);
  } else if (inputrec->eI == eiMC) {
     for(i=0;i<state->natoms;i++)
      copy_rvec(state->x[i],xprime[i]);

     if (!(mc_move->update_box)) {
      if(PAR(cr)) { 
       if(DOMAINDECOMP(cr)) {
       }
       else {
       for(i=md->start;i<(md->start+md->homenr);i++) {
        if(i >= mc_move->start && i <mc_move->end) {
//         do_update_mc(xprime[i],&(state->mc_move));
        }
       }
      }
     }
     else {
      do_update_mc(xprime,md->massA,mc_move,graph,rng,md->homenr);
     }
    }
  } else {
    gmx_fatal(FARGS,"Don't know how to update coordinates");
  }
  where();
  inc_nrnb(nrnb, (bNH || bPR) ? eNR_EXTUPDATE : eNR_UPDATE, homenr);
  dump_it_all(fplog,"After update",
	      state->natoms,state->x,xprime,state->v,force);

  /* 
   *  Steps (7C, 8C)
   *  APPLY CONSTRAINTS:
   *  BLOCK SHAKE 
   */

    /* When doing PR pressure coupling we have to constrain the
     * bonds in each iteration. If we are only using Nose-Hoover tcoupling
     * it is enough to do this once though, since the relative velocities 
     * after this will be normal to the bond vector
     */
    if (constr)
    {
        bLastStep = (step == inputrec->init_step+inputrec->nsteps);
        bLog  = (do_per_step(step,inputrec->nstlog) || bLastStep || (step < 0));
        bEner = (do_per_step(step,inputrec->nstenergy) || bLastStep);
        if (constr)
        {
            /* Constrain the coordinates xprime */
            wallcycle_start(wcycle,ewcCONSTR);
            constrain(NULL,bLog,bEner,constr,idef,
                      inputrec,cr,step,1,md,
                      state->x,xprime,NULL,
                      state->box,state->lambda,dvdlambda,
                      state->v,bCalcVir ? &vir_con : NULL,nrnb,econqCoord);
            wallcycle_stop(wcycle,ewcCONSTR);
        }
        where();
        
        dump_it_all(fplog,"After Shake",
                    state->natoms,state->x,xprime,state->v,force);
        
        if (bCalcVir)
        {
            if (inputrec->eI == eiSD2)
            {
                /* A correction factor eph is needed for the SD constraint force */
                /* Here we can, unfortunately, not have proper corrections
                 * for different friction constants, so we use the first one.
                 */
                eph = upd->sd->sdc[0].eph;
                for(i=0; i<DIM; i++)
                    for(m=0; m<DIM; m++)
                        vir_part[i][m] += eph*vir_con[i][m];
            }
            else
            {
                m_add(vir_part,vir_con,vir_part);
            }
            if (debug)
            {
                pr_rvecs(debug,0,"constraint virial",vir_part,DIM);
            }
        }
        where();
    }
  
    where();
    if (inputrec->eI == eiSD2)
    {
        /* The second part of the SD integration */
        do_update_sd2(upd->sd,FALSE,start,homenr,
                      inputrec->opts.acc,inputrec->opts.nFreeze,
                      md->invmass,md->ptype,
                      md->cFREEZE,md->cACC,md->cTC,
                      state->x,xprime,state->v,force,state->sd_X,
                      inputrec->opts.ngtc,inputrec->opts.tau_t,inputrec->opts.ref_t,
                      FALSE);
        inc_nrnb(nrnb, eNR_UPDATE, homenr);
        
        if (constr) {
            /* Constrain the coordinates xprime */
            wallcycle_start(wcycle,ewcCONSTR);
            constrain(NULL,bLog,bEner,constr,idef,
                      inputrec,cr,step,1,md,
                      state->x,xprime,NULL,
                      state->box,state->lambda,dvdlambda,
                      NULL,NULL,nrnb,econqCoord);
            wallcycle_stop(wcycle,ewcCONSTR);
        }
    }
  
  /* We must always unshift here, also if we did not shake
   * x was shifted in do_force */
  
  if (graph && (graph->nnodes > 0)) { 
    unshift_x(graph,state->box,state->x,xprime);
    if (TRICLINIC(state->box))
      inc_nrnb(nrnb,eNR_SHIFTX,2*graph->nnodes);
    else
      inc_nrnb(nrnb,eNR_SHIFTX,graph->nnodes);    
    for(n=start; (n<graph->start); n++)
      copy_rvec(xprime[n],state->x[n]);
    for(n=graph->start+graph->nnodes; (n<start+homenr); n++)
      copy_rvec(xprime[n],state->x[n]);
  } else {
    for(n=start; (n<start+homenr); n++)
      copy_rvec(xprime[n],state->x[n]);
  }
  dump_it_all(fplog,"After unshift",
	      state->natoms,state->x,xprime,state->v,force);
  where();

  update_ekindata(start,homenr,ekind,&(inputrec->opts),state->v,md,
		  state->lambda,bNEMD);

  if (bCouple && inputrec->epc != epcNO) {
    if (inputrec->epc == epcBERENDSEN) {
        berendsen_pscale(inputrec,pcoupl_mu,state->box,state->box_rel,
                         start,homenr,state->x,md->cFREEZE,nrnb);
    } else if(inputrec->epc == epcMC && state->mc_move->update_box) {

     mc_pscale(inputrec,pcoupl_mu,state->box,state->box_rel,
		       start,homenr,state->x,md->cFREEZE,nrnb,mols,mc_move->xcm,graph);
    } else if (inputrec->epc == epcPARRINELLORAHMAN) {
      /* The box velocities were updated in do_pr_pcoupl in the update
       * iteration, but we dont change the box vectors until we get here
       * since we need to be able to shift/unshift above.
       */
      for(i=0;i<DIM;i++)
	for(m=0;m<=i;m++)
	  state->box[i][m] += dt*state->boxv[i][m];
      
      preserve_box_shape(inputrec,state->box_rel,state->box);

      /* Scale the coordinates */
      for(n=start; (n<start+homenr); n++) {
	tmvmul_ur0(pcoupl_mu,state->x[n],state->x[n]);
      }
    }
    if (scale_tot) {
      /* The transposes of the scaling matrices are stored,
       * therefore we need to reverse the order in the multiplication.
       */
      mmul_ur0(*scale_tot,pcoupl_mu,*scale_tot);
    }
  }
  if (DEFORM(*inputrec)) {
      deform(upd,start,homenr,state->x,state->box,scale_tot,inputrec,step);
  }
  where();
}


void correct_ekin(FILE *log,int start,int end,rvec v[],rvec vcm,real mass[],
                  real tmass,tensor ekin)
{
  /* 
   * This is a debugging routine. It should not be called for production code
   *
   * The kinetic energy should calculated according to:
   *   Ekin = 1/2 m (v-vcm)^2
   * However the correction is not always applied, since vcm may not be
   * known in time and we compute
   *   Ekin' = 1/2 m v^2 instead
   * This can be corrected afterwards by computing
   *   Ekin = Ekin' + 1/2 m ( -2 v vcm + vcm^2)
   * or in hsorthand:
   *   Ekin = Ekin' - m v vcm + 1/2 m vcm^2
   */
  int    i,j,k;
  real   m,tm;
  rvec   hvcm,mv;
  tensor dekin;

  /* Local particles */  
  clear_rvec(mv);

  /* Processor dependent part. */
  tm = 0;
  for(i=start; (i<end); i++) {
    m      = mass[i];
    tm    += m;
    for(j=0; (j<DIM); j++)
      mv[j] += m*v[i][j];
  }
  /* Shortcut */ 
  svmul(1/tmass,vcm,vcm); 
  svmul(0.5,vcm,hvcm);
  clear_mat(dekin);
  for(j=0; (j<DIM); j++)
    for(k=0; (k<DIM); k++)
      dekin[j][k] += vcm[k]*(tm*hvcm[j]-mv[j]);

  pr_rvecs(log,0,"dekin",dekin,DIM);
  pr_rvecs(log,0," ekin", ekin,DIM);
  fprintf(log,"dekin = %g, ekin = %g  vcm = (%8.4f %8.4f %8.4f)\n",
          trace(dekin),trace(ekin),vcm[XX],vcm[YY],vcm[ZZ]);
  fprintf(log,"mv = (%8.4f %8.4f %8.4f)\n",
          mv[XX],mv[YY],mv[ZZ]);
}
