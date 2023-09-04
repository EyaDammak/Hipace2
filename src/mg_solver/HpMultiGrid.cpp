/* Copyright 2022
 *
 * This file is part of HiPACE++.
 *
 * Authors: Weiqun Zhang
 * License: BSD-3-Clause-LBNL
 */
#include "HpMultiGrid.H"
#include <algorithm>

using namespace amrex;

namespace hpmg {

namespace {

constexpr int n_cell_single = 32; // switch to single block when box is smaller than this

Box valid_domain_box (Box const& domain)
{
    return domain.cellCentered() ? domain : amrex::grow(domain, IntVect(-1,-1,0));
}

template <typename T, typename U>
AMREX_GPU_DEVICE AMREX_FORCE_INLINE
void restrict_cc (int i, int j, int n, Array4<T> const& crse, Array4<U> const& fine)
{
    crse(i,j,0,n) = Real(0.25)*(fine(2*i  ,2*j  ,0,n) +
                                fine(2*i+1,2*j  ,0,n) +
                                fine(2*i  ,2*j+1,0,n) +
                                fine(2*i+1,2*j+1,0,n));
}

template <typename T, typename U>
AMREX_GPU_DEVICE AMREX_FORCE_INLINE
void restrict_nd (int i, int j, int n, Array4<T> const& crse, Array4<U> const& fine)
{
    crse(i,j,0,n) = Real(1./16.) * (fine(2*i-1,2*j-1,0,n) +
                           Real(2.)*fine(2*i  ,2*j-1,0,n) +
                                    fine(2*i+1,2*j-1,0,n) +
                           Real(2.)*fine(2*i-1,2*j  ,0,n) +
                           Real(4.)*fine(2*i  ,2*j  ,0,n) +
                           Real(2.)*fine(2*i+1,2*j  ,0,n) +
                                    fine(2*i-1,2*j+1,0,n) +
                           Real(2.)*fine(2*i  ,2*j+1,0,n) +
                                    fine(2*i+1,2*j+1,0,n));
}

template <typename T, typename U>
AMREX_GPU_DEVICE AMREX_FORCE_INLINE
void interpadd_cc (int i, int j, int n, Array4<T> const& fine, Array4<U> const& crse)
{
    int ic = amrex::coarsen(i,2);
    int jc = amrex::coarsen(j,2);
    fine(i,j,0,n) += crse(ic,jc,0,n);
}

template <typename T, typename U>
AMREX_GPU_DEVICE AMREX_FORCE_INLINE
void interpadd_nd (int i, int j, int n, Array4<T> const& fine, Array4<U> const& crse)
{
    int ic = amrex::coarsen(i,2);
    int jc = amrex::coarsen(j,2);
    bool i_is_odd = (ic*2 != i);
    bool j_is_odd = (jc*2 != j);
    if (i_is_odd && j_is_odd) {
        fine(i,j,0,n) += (crse(ic  ,jc  ,0,n) +
                          crse(ic+1,jc  ,0,n) +
                          crse(ic  ,jc+1,0,n) +
                          crse(ic+1,jc+1,0,n))*Real(0.25);
    } else if (i_is_odd) {
        fine(i,j,0,n) += (crse(ic  ,jc,0,n) +
                          crse(ic+1,jc,0,n))*Real(0.5);
    } else if (j_is_odd) {
        fine(i,j,0,n) += (crse(ic,jc  ,0,n) +
                          crse(ic,jc+1,0,n))*Real(0.5);
    } else {
        fine(i,j,0,n) += crse(ic,jc,0,n);
    }
}

template <typename T, typename U, typename V>
AMREX_GPU_DEVICE AMREX_FORCE_INLINE
void interpcpy_cc (int i, int j, int n, Array4<T> const& fine_in, Array4<U> const& crse,
                Array4<V> const& fine_out)
{
    int ic = amrex::coarsen(i,2);
    int jc = amrex::coarsen(j,2);
    fine_out(i,j,0,n) = fine_in(i,j,0,n) + crse(ic,jc,0,n);
}

template <typename T, typename U, typename V>
AMREX_GPU_DEVICE AMREX_FORCE_INLINE
void interpcpy_nd (int i, int j, int n, Array4<T> const& fine_in, Array4<U> const& crse,
                Array4<V> const& fine_out)
{
    int ic = amrex::coarsen(i,2);
    int jc = amrex::coarsen(j,2);
    bool i_is_odd = (ic*2 != i);
    bool j_is_odd = (jc*2 != j);
    if (i_is_odd && j_is_odd) {
        fine_out(i,j,0,n) = fine_in(i,j,0,n) + (crse(ic  ,jc  ,0,n) +
                                                crse(ic+1,jc  ,0,n) +
                                                crse(ic  ,jc+1,0,n) +
                                                crse(ic+1,jc+1,0,n))*Real(0.25);
    } else if (i_is_odd) {
        fine_out(i,j,0,n) = fine_in(i,j,0,n) + (crse(ic  ,jc,0,n) +
                                                crse(ic+1,jc,0,n))*Real(0.5);
    } else if (j_is_odd) {
        fine_out(i,j,0,n) = fine_in(i,j,0,n) + (crse(ic,jc  ,0,n) +
                                                crse(ic,jc+1,0,n))*Real(0.5);
    } else {
        fine_out(i,j,0,n) = fine_in(i,j,0,n) +  crse(ic,jc,0,n);
    }
}

AMREX_GPU_DEVICE AMREX_FORCE_INLINE
Real laplacian (int i, int j, int n, int ilo, int jlo, int ihi, int jhi,
                Array4<Real> const& phi, Real facx, Real facy)
{
    Real lap = Real(-2.)*(facx+facy)*phi(i,j,0,n);
    if (i == ilo) {
        lap += facx * (Real(4./3.)*phi(i+1,j,0,n) - Real(2.)*phi(i,j,0,n));
    } else if (i == ihi) {
        lap += facx * (Real(4./3.)*phi(i-1,j,0,n) - Real(2.)*phi(i,j,0,n));
    } else {
        lap += facx * (phi(i-1,j,0,n) + phi(i+1,j,0,n));
    }
    if (j == jlo) {
        lap += facy * (Real(4./3.)*phi(i,j+1,0,n) - Real(2.)*phi(i,j,0,n));
    } else if (j == jhi) {
        lap += facy * (Real(4./3.)*phi(i,j-1,0,n) - Real(2.)*phi(i,j,0,n));
    } else {
        lap += facy * (phi(i,j-1,0,n) + phi(i,j+1,0,n));
    }
    return lap;
}

AMREX_GPU_DEVICE AMREX_FORCE_INLINE
Real residual1 (int i, int j, int n, int ilo, int jlo, int ihi, int jhi,
                Array4<Real> const& phi, Real rhs, Real acf, Real facx, Real facy)
{
    Real lap = laplacian(i,j,n,ilo,jlo,ihi,jhi,phi,facx,facy);
    return rhs + acf*phi(i,j,0,n) - lap;
}

AMREX_GPU_DEVICE AMREX_FORCE_INLINE
Real residual2r (int i, int j, int ilo, int jlo, int ihi, int jhi,
                 Array4<Real> const& phi, Real rhs, Real acf_r, Real acf_i,
                 Real facx, Real facy)
{
    Real lap = laplacian(i,j,0,ilo,jlo,ihi,jhi,phi,facx,facy);
    return rhs + acf_r*phi(i,j,0,0) - acf_i*phi(i,j,0,1) - lap;
}

AMREX_GPU_DEVICE AMREX_FORCE_INLINE
Real residual2i (int i, int j, int ilo, int jlo, int ihi, int jhi,
                 Array4<Real> const& phi, Real rhs, Real acf_r, Real acf_i,
                 Real facx, Real facy)
{
    Real lap = laplacian(i,j,1,ilo,jlo,ihi,jhi,phi,facx,facy);
    return rhs + acf_i*phi(i,j,0,0) + acf_r*phi(i,j,0,1) - lap;
}

// res = rhs - L(phi)
void compute_residual (Box const& box, Array4<Real> const& res,
                       Array4<Real> const& phi, Array4<Real const> const& rhs,
                       Array4<Real const> const& acf, Real dx, Real dy,
                       int system_type)
{
    int const ilo = box.smallEnd(0);
    int const jlo = box.smallEnd(1);
    int const ihi = box.bigEnd(0);
    int const jhi = box.bigEnd(1);
    Real facx = Real(1.)/(dx*dx);
    Real facy = Real(1.)/(dy*dy);
    if (system_type == 1) {
        hpmg::ParallelFor(valid_domain_box(box), 2,
        [=] AMREX_GPU_DEVICE (int i, int j, int, int n) noexcept
        {
            res(i,j,0,n) = residual1(i, j, n, ilo, jlo, ihi, jhi, phi, rhs(i,j,0,n),
                                     acf(i,j,0), facx, facy);
        });
    } else {
        hpmg::ParallelFor(valid_domain_box(box),
        [=] AMREX_GPU_DEVICE (int i, int j, int) noexcept
        {
            res(i,j,0,0) = residual2r(i, j, ilo, jlo, ihi, jhi, phi, rhs(i,j,0,0),
                                      acf(i,j,0,0), acf(i,j,0,1), facx, facy);
            res(i,j,0,1) = residual2i(i, j, ilo, jlo, ihi, jhi, phi, rhs(i,j,0,1),
                                      acf(i,j,0,0), acf(i,j,0,1), facx, facy);
        });
    }
}

void compute_residual_shared (Box const& box, Array4<Real> const& res,
                       Array4<Real> const& phi, Array4<Real const> const& rhs,
                       Array4<Real const> const& acf, Real dx, Real dy,
                       int)
{
    constexpr int tilesize = 32;
    constexpr int tilesize_array = tilesize + 2;

    int const ilo = box.smallEnd(0);
    int const jlo = box.smallEnd(1);
    int const ihi = box.bigEnd(0);
    int const jhi = box.bigEnd(1);
    Real facx = Real(1.)/(dx*dx);
    Real facy = Real(1.)/(dy*dy);

    const Box loop_box = valid_domain_box(box);
    int const ilo_loop = loop_box.smallEnd(0);
    int const jlo_loop = loop_box.smallEnd(1);
    int const ihi_loop = loop_box.bigEnd(0);
    int const jhi_loop = loop_box.bigEnd(1);
    const int num_blocks_x = (loop_box.length(0) + tilesize - 1)/tilesize;
    const int num_blocks_y = (loop_box.length(1) + tilesize - 1)/tilesize;

    amrex::launch<tilesize*tilesize>(num_blocks_x*num_blocks_y, amrex::Gpu::gpuStream(),
        [=] AMREX_GPU_DEVICE() noexcept
        {
            __shared__ Real phi_ptr[tilesize_array*tilesize_array*2];

            const int iblock_y = blockIdx.x / num_blocks_x;
            const int iblock_x = blockIdx.x - iblock_y * num_blocks_x;

            const int tile_begin_x = iblock_x * tilesize - 1 + ilo_loop;
            const int tile_begin_y = iblock_y * tilesize - 1 + jlo_loop;

            const int tile_end_x = iblock_x * tilesize - 1 + ilo_loop + tilesize_array;
            const int tile_end_y = iblock_y * tilesize - 1 + jlo_loop + tilesize_array;

            Array4<Real> phi_shared(phi_ptr, {tile_begin_x,tile_begin_y,0},
                                             {tile_end_x,tile_end_y,1}, 2);

            for (int s = threadIdx.x; s < tilesize_array*tilesize_array; s+=blockDim.x) {
                int sy = s / tilesize_array;
                int sx = s - sy * tilesize_array;
                sx += tile_begin_x;
                sy += tile_begin_y;
                if (ilo_loop <= sx && sx <= ihi_loop &&
                    jlo_loop <= sy && sy <= jhi_loop) {
                    phi_shared(sx, sy, 0, 0) = phi(sx, sy, 0, 0);
                    phi_shared(sx, sy, 0, 1) = phi(sx, sy, 0, 1);
                } else {
                    phi_shared(sx, sy, 0, 0) = Real(0.);
                    phi_shared(sx, sy, 0, 1) = Real(0.);
                }
            }

            __syncthreads();

            const int ithread_y = threadIdx.x / tilesize;
            const int ithread_x = threadIdx.x - ithread_y * tilesize;

            const int i = iblock_x * tilesize + ithread_x + ilo_loop;
            const int j = iblock_y * tilesize + ithread_y + jlo_loop;

            if (ilo_loop <= i && i <= ihi_loop &&
                jlo_loop <= j && j <= jhi_loop) {
                res(i,j,0,0) = residual1(i, j, 0, ilo, jlo, ihi, jhi, phi_shared, rhs(i,j,0,0),
                                         acf(i,j,0), facx, facy);
                res(i,j,0,1) = residual1(i, j, 1, ilo, jlo, ihi, jhi, phi_shared, rhs(i,j,0,1),
                                         acf(i,j,0), facx, facy);
            }
        });
}

template<bool is_cell_centered = true>
AMREX_GPU_DEVICE AMREX_FORCE_INLINE
void gs1 (int i, int j, int n, int ilo, int jlo, int ihi, int jhi,
          Array4<Real> const& phi, Real rhs, Real acf, Real facx, Real facy)
{
    Real lap;
    Real c0 = -(acf+Real(2.)*(facx+facy));
    if (is_cell_centered && i == ilo) {
        lap = facx * Real(4./3.)*phi(i+1,j,0,n);
        c0 -= Real(2.)*facx;
    } else if (is_cell_centered && i == ihi) {
        lap = facx * Real(4./3.)*phi(i-1,j,0,n);
        c0 -= Real(2.)*facx;
    } else {
        lap = facx * (phi(i-1,j,0,n) + phi(i+1,j,0,n));
    }
    if (is_cell_centered && j == jlo) {
        lap += facy * Real(4./3.)*phi(i,j+1,0,n);
        c0 -= Real(2.)*facy;
    } else if (is_cell_centered && j == jhi) {
        lap += facy * Real(4./3.)*phi(i,j-1,0,n);
        c0 -= Real(2.)*facy;
    } else {
        lap += facy * (phi(i,j-1,0,n) + phi(i,j+1,0,n));
    }
    const Real c0_inv = Real(1.) / c0;
    phi(i,j,0,n) = (rhs - lap) * c0_inv;
}

AMREX_GPU_DEVICE AMREX_FORCE_INLINE
void gs2 (int i, int j, int ilo, int jlo, int ihi, int jhi,
          Array4<Real> const& phi, Real rhs_r, Real rhs_i,
          Real ar, Real ai, Real facx, Real facy)
{
    Real lap[2];
    Real c0 = Real(-2.)*(facx+facy);
    if (i == ilo) {
        lap[0] = facx * Real(4./3.)*phi(i+1,j,0,0);
        lap[1] = facx * Real(4./3.)*phi(i+1,j,0,1);
        c0 -= Real(2.)*facx;
    } else if (i == ihi) {
        lap[0] = facx * Real(4./3.)*phi(i-1,j,0,0);
        lap[1] = facx * Real(4./3.)*phi(i-1,j,0,1);
        c0 -= Real(2.)*facx;
    } else {
        lap[0] = facx * (phi(i-1,j,0,0) + phi(i+1,j,0,0));
        lap[1] = facx * (phi(i-1,j,0,1) + phi(i+1,j,0,1));
    }
    if (j == jlo) {
        lap[0] += facy * Real(4./3.)*phi(i,j+1,0,0);
        lap[1] += facy * Real(4./3.)*phi(i,j+1,0,1);
        c0 -= Real(2.)*facy;
    } else if (j == jhi) {
        lap[0] += facy * Real(4./3.)*phi(i,j-1,0,0);
        lap[1] += facy * Real(4./3.)*phi(i,j-1,0,1);
        c0 -= Real(2.)*facy;
    } else {
        lap[0] += facy * (phi(i,j-1,0,0) + phi(i,j+1,0,0));
        lap[1] += facy * (phi(i,j-1,0,1) + phi(i,j+1,0,1));
    }
    Real c[2] = {c0-ar, -ai};
    Real cmag = Real(1.)/(c[0]*c[0] + c[1]*c[1]);
    phi(i,j,0,0) = ((rhs_r-lap[0])*c[0] + (rhs_i-lap[1])*c[1]) * cmag;
    phi(i,j,0,1) = ((rhs_i-lap[1])*c[0] - (rhs_r-lap[0])*c[1]) * cmag;
}

void gsrb (int icolor, Box const& box, Array4<Real> const& phi,
           Array4<Real const> const& rhs, Array4<Real const> const& acf,
           Real dx, Real dy, int system_type)
{
    int const ilo = box.smallEnd(0);
    int const jlo = box.smallEnd(1);
    int const ihi = box.bigEnd(0);
    int const jhi = box.bigEnd(1);
    Real facx = Real(1.)/(dx*dx);
    Real facy = Real(1.)/(dy*dy);
    if (system_type == 1) {
        hpmg::ParallelFor(valid_domain_box(box),
        [=] AMREX_GPU_DEVICE (int i, int j, int) noexcept
        {
            if ((i+j+icolor)%2 == 0) {
                gs1(i, j, 0, ilo, jlo, ihi, jhi, phi, rhs(i,j,0,0), acf(i,j,0), facx, facy);
                gs1(i, j, 1, ilo, jlo, ihi, jhi, phi, rhs(i,j,0,1), acf(i,j,0), facx, facy);
            }
        });
    } else {
        hpmg::ParallelFor(valid_domain_box(box),
        [=] AMREX_GPU_DEVICE (int i, int j, int) noexcept
        {
            if ((i+j+icolor)%2 == 0) {
                gs2(i, j, ilo, jlo, ihi, jhi, phi, rhs(i,j,0,0), rhs(i,j,0,1),
                    acf(i,j,0,0), acf(i,j,0,1), facx, facy);
            }
        });
    }
}

// old: 38.03
// 1: 32.42
// 2: 29.94
// 2: 22.03
// ...
// old: 32.61
// 4i: 18.76
// 6i: 24.69
// 2i: 20.62
// 3i: 19.66
// 5i: 21.48
// ...
// old: 32.6
// 1: 18.76
// 2: 18.76
// 3: 18.74
// 4: 18.86
// 5: 18.73
// 6: 18.97
// 7: 17.53
// 8: 17.52
// 9: 19.09
//10: 17.45
//11: 16.29
//12: 17.24
//13: 16.29
//14: 16.66
//15: 15.77
//16: 15.9
//15: 15.76
//16: 15.47
//17: 15.46
//18: 15.07
//19: 15.29
//20: 15.12
//21: 16.71
//22: 15.1
//23: 14.89
//24: 14.93
//25: 15.56
//26: 14.94
//27: 14.94
//28: 14.44
//29: 14.45
//30: 14.44

void gsrb_shared_st1_4_up (Box const& box, Array4<Real> const& phi,
                           Array4<Real const> const& rhs, Array4<Real const> const& acf,
                           Real dx, Real dy)
{
    constexpr int tilesize = 32;
    constexpr int tilesize_array = tilesize + 2;
    constexpr int niter = 4;
    constexpr int edge_offset = niter - 1;
    constexpr int final_tilesize = tilesize - 2*edge_offset;

    int const ilo = box.smallEnd(0);
    int const jlo = box.smallEnd(1);
    int const ihi = box.bigEnd(0);
    int const jhi = box.bigEnd(1);
    Real facx = Real(1.)/(dx*dx);
    Real facy = Real(1.)/(dy*dy);

    const Box loop_box = valid_domain_box(box);
    int const ilo_loop = loop_box.smallEnd(0);
    int const jlo_loop = loop_box.smallEnd(1);
    int const ihi_loop = loop_box.bigEnd(0);
    int const jhi_loop = loop_box.bigEnd(1);
    const int num_blocks_x = (loop_box.length(0) + final_tilesize - 1)/final_tilesize;
    const int num_blocks_y = (loop_box.length(1) + final_tilesize - 1)/final_tilesize;

    amrex::launch<tilesize*tilesize>(num_blocks_x*num_blocks_y, amrex::Gpu::gpuStream(),
        [=] AMREX_GPU_DEVICE() noexcept
        {
            __shared__ Real phi_ptr[tilesize_array*tilesize_array*2];

            for (int s = threadIdx.x; s < tilesize_array*tilesize_array*2; s+=blockDim.x) {
                phi_ptr[s] = Real(0.);
            }

            __syncthreads();

            const int iblock_y = blockIdx.x / num_blocks_x;
            const int iblock_x = blockIdx.x - iblock_y * num_blocks_x;

            const int ithread_y = threadIdx.x / tilesize;
            const int ithread_x = threadIdx.x - ithread_y * tilesize;

            const int i = iblock_x * final_tilesize + ithread_x - edge_offset + ilo_loop;
            const int j = iblock_y * final_tilesize + ithread_y - edge_offset + jlo_loop;

            const int tile_begin_x = iblock_x * final_tilesize - niter + ilo_loop;
            const int tile_begin_y = iblock_y * final_tilesize - niter + jlo_loop;

            const int tile_end_x = iblock_x * final_tilesize - niter + ilo_loop + tilesize_array;
            const int tile_end_y = iblock_y * final_tilesize - niter + jlo_loop + tilesize_array;

            Array4<Real> phi_shared(phi_ptr, {tile_begin_x,tile_begin_y,0},
                                             {tile_end_x,tile_end_y,1}, 2);

            Real rhs0_num = Real(0.);
            Real rhs1_num = Real(0.);
            Real acf_num = Real(0.);

            if (ilo_loop <= i && i <= ihi_loop &&
                jlo_loop <= j && j <= jhi_loop) {
                rhs0_num = rhs(i, j, 0, 0);
                rhs1_num = rhs(i, j, 0, 1);
                acf_num = acf(i, j, 0);
            }

            for (int icolor=0; icolor<niter; ++icolor) {
                if (ilo_loop <= i && i <= ihi_loop &&
                    jlo_loop <= j && j <= jhi_loop &&
                    (i+j+icolor)%2 == 0) {
                    gs1(i, j, 0, ilo, jlo, ihi, jhi, phi_shared, rhs0_num, acf_num, facx, facy);
                    gs1(i, j, 1, ilo, jlo, ihi, jhi, phi_shared, rhs1_num, acf_num, facx, facy);
                }
                __syncthreads();
            }

            if (ilo_loop <= i && i <= ihi_loop &&
                jlo_loop <= j && j <= jhi_loop &&
                edge_offset <= ithread_x && ithread_x < tilesize - edge_offset &&
                edge_offset <= ithread_y && ithread_y < tilesize - edge_offset) {
                phi(i, j, 0, 0) = phi_shared(i, j, 0, 0);
                phi(i, j, 0, 1) = phi_shared(i, j, 0, 1);
            }
        });
}

void gsrb_shared_st1_4_up_v3 (Box const& box, Array4<Real> const& phi,
                              Array4<Real const> const& rhs, Array4<Real const> const& acf,
                              Real dx, Real dy, Array4<Real> const& res)
{
    constexpr int tilesize = 32;
    constexpr int tilesize_array = tilesize + 2;
    constexpr int niter = 4;
    constexpr int edge_offset = niter;
    constexpr int final_tilesize = tilesize - 2*edge_offset;

    int const ilo = box.smallEnd(0);
    int const jlo = box.smallEnd(1);
    int const ihi = box.bigEnd(0);
    int const jhi = box.bigEnd(1);
    Real facx = Real(1.)/(dx*dx);
    Real facy = Real(1.)/(dy*dy);

    const Box loop_box = valid_domain_box(box);
    int const ilo_loop = loop_box.smallEnd(0);
    int const jlo_loop = loop_box.smallEnd(1);
    int const ihi_loop = loop_box.bigEnd(0);
    int const jhi_loop = loop_box.bigEnd(1);
    const int num_blocks_x = (loop_box.length(0) + final_tilesize - 1)/final_tilesize;
    const int num_blocks_y = (loop_box.length(1) + final_tilesize - 1)/final_tilesize;

    amrex::launch<tilesize*tilesize>(num_blocks_x*num_blocks_y, amrex::Gpu::gpuStream(),
        [=] AMREX_GPU_DEVICE() noexcept
        {
            __shared__ Real phi_ptr[tilesize_array*tilesize_array*2];

            for (int s = threadIdx.x; s < tilesize_array*tilesize_array*2; s+=blockDim.x) {
                phi_ptr[s] = Real(0.);
            }

            __syncthreads();

            const int iblock_y = blockIdx.x / num_blocks_x;
            const int iblock_x = blockIdx.x - iblock_y * num_blocks_x;

            const int ithread_y = threadIdx.x / tilesize;
            const int ithread_x = threadIdx.x - ithread_y * tilesize;

            const int i = iblock_x * final_tilesize + ithread_x - edge_offset + ilo_loop;
            const int j = iblock_y * final_tilesize + ithread_y - edge_offset + jlo_loop;

            const int tile_begin_x = iblock_x * final_tilesize - edge_offset - 1 + ilo_loop;
            const int tile_begin_y = iblock_y * final_tilesize - edge_offset - 1 + jlo_loop;

            const int tile_end_x = iblock_x * final_tilesize - edge_offset - 1 + ilo_loop + tilesize_array;
            const int tile_end_y = iblock_y * final_tilesize - edge_offset - 1 + jlo_loop + tilesize_array;

            Array4<Real> phi_shared(phi_ptr, {tile_begin_x,tile_begin_y,0},
                                             {tile_end_x,tile_end_y,1}, 2);

            Real rhs0_num = Real(0.);
            Real rhs1_num = Real(0.);
            Real acf_num = Real(0.);

            if (ilo_loop <= i && i <= ihi_loop &&
                jlo_loop <= j && j <= jhi_loop) {
                rhs0_num = rhs(i, j, 0, 0);
                rhs1_num = rhs(i, j, 0, 1);
                acf_num = acf(i, j, 0);
            }

            for (int icolor=0; icolor<niter; ++icolor) {
                if (ilo_loop <= i && i <= ihi_loop &&
                    jlo_loop <= j && j <= jhi_loop &&
                    (i+j+icolor)%2 == 0) {
                    gs1(i, j, 0, ilo, jlo, ihi, jhi, phi_shared, rhs0_num, acf_num, facx, facy);
                    gs1(i, j, 1, ilo, jlo, ihi, jhi, phi_shared, rhs1_num, acf_num, facx, facy);
                }
                __syncthreads();
            }

            if (ilo_loop <= i && i <= ihi_loop &&
                jlo_loop <= j && j <= jhi_loop &&
                edge_offset <= ithread_x && ithread_x < tilesize - edge_offset &&
                edge_offset <= ithread_y && ithread_y < tilesize - edge_offset) {

                res(i, j, 0, 0) = residual1(i, j, 0, ilo, jlo, ihi, jhi,
                                            phi_shared, rhs0_num, acf_num, facx, facy);
                res(i, j, 0, 1) = residual1(i, j, 1, ilo, jlo, ihi, jhi,
                                            phi_shared, rhs1_num, acf_num, facx, facy);

                phi(i, j, 0, 0) = phi_shared(i, j, 0, 0);
                phi(i, j, 0, 1) = phi_shared(i, j, 0, 1);
            }
        });
}

void gsrb_shared_st1_4_up_v5 (Box const& box, Array4<Real> const& phi,
                              Array4<Real const> const& rhs, Array4<Real const> const& acf,
                              Real dx, Real dy, Array4<Real> const& res)
{
    constexpr int tilesize_x = 64;
    constexpr int tilesize_y = 32;
    constexpr int thread_tilesize = 32;
    constexpr int tilesize_array_x = tilesize_x + 2;
    constexpr int tilesize_array_y = tilesize_y + 2;
    constexpr int niter = 4;
    constexpr int edge_offset = niter;
    constexpr int final_tilesize_x = tilesize_x - 2*edge_offset;
    constexpr int final_tilesize_y = tilesize_y - 2*edge_offset;

    int const ilo = box.smallEnd(0);
    int const jlo = box.smallEnd(1);
    int const ihi = box.bigEnd(0);
    int const jhi = box.bigEnd(1);
    Real facx = Real(1.)/(dx*dx);
    Real facy = Real(1.)/(dy*dy);

    const Box loop_box = valid_domain_box(box);
    int const ilo_loop = loop_box.smallEnd(0);
    int const jlo_loop = loop_box.smallEnd(1);
    int const ihi_loop = loop_box.bigEnd(0);
    int const jhi_loop = loop_box.bigEnd(1);
    const int num_blocks_x = (loop_box.length(0) + final_tilesize_x - 1)/final_tilesize_x;
    const int num_blocks_y = (loop_box.length(1) + final_tilesize_y - 1)/final_tilesize_y;

    amrex::launch<thread_tilesize*thread_tilesize>(num_blocks_x*num_blocks_y, amrex::Gpu::gpuStream(),
        [=] AMREX_GPU_DEVICE() noexcept
        {
            __shared__ Real phi_ptr[tilesize_array_x*tilesize_array_y*2];

            for (int s = threadIdx.x; s < tilesize_array_x*tilesize_array_y*2; s+=blockDim.x) {
                phi_ptr[s] = Real(0.);
            }

            __syncthreads();

            const int iblock_y = blockIdx.x / num_blocks_x;
            const int iblock_x = blockIdx.x - iblock_y * num_blocks_x;

            int ithread_y = threadIdx.x / tilesize_x;
            const int ithread_x = threadIdx.x - ithread_y * tilesize_x;
            ithread_y *= 2;

            const int i = iblock_x * final_tilesize_x + ithread_x - edge_offset + ilo_loop;
            const int j = iblock_y * final_tilesize_y + ithread_y - edge_offset + jlo_loop;

            const int tile_begin_x = iblock_x * final_tilesize_x - edge_offset - 1 + ilo_loop;
            const int tile_begin_y = iblock_y * final_tilesize_y - edge_offset - 1 + jlo_loop;

            const int tile_end_x = iblock_x * final_tilesize_x - edge_offset - 1 + ilo_loop + tilesize_array_x;
            const int tile_end_y = iblock_y * final_tilesize_y - edge_offset - 1 + jlo_loop + tilesize_array_y;

            Array4<Real> phi_shared(phi_ptr, {tile_begin_x,tile_begin_y,0},
                                             {tile_end_x,tile_end_y,1}, 2);

            Real rhs0_num[2] = {0., 0.};
            Real rhs1_num[2] = {0., 0.};
            Real acf_num[2] = {0., 0.};

            for (int nj=0; nj<=1; ++nj) {
                if (ilo_loop <= i && i <= ihi_loop &&
                    jlo_loop <= j+nj && j+nj <= jhi_loop) {
                    rhs0_num[nj] = rhs(i, j+nj, 0, 0);
                    rhs1_num[nj] = rhs(i, j+nj, 0, 1);
                    acf_num[nj] = acf(i, j+nj, 0);
                }
            }

            for (int icolor=0; icolor<niter; ++icolor) {
                const int shift = (i + j + icolor) % 2;
                const int j_loc = j + shift;
                const Real rhs0_loc = shift ? rhs0_num[1] : rhs0_num[0];
                const Real rhs1_loc = shift ? rhs1_num[1] : rhs1_num[0];
                const Real acf_loc = shift ? acf_num[1] : acf_num[0];
                if (ilo_loop <= i && i <= ihi_loop &&
                    jlo_loop <= j_loc && j_loc <= jhi_loop) {
                    gs1(i, j_loc, 0, ilo, jlo, ihi, jhi, phi_shared, rhs0_loc, acf_loc, facx, facy);
                    gs1(i, j_loc, 1, ilo, jlo, ihi, jhi, phi_shared, rhs1_loc, acf_loc, facx, facy);
                }
                __syncthreads();
            }

            for (int nj=0; nj<=1; ++nj) {
                if (ilo_loop <= i && i <= ihi_loop &&
                    jlo_loop <= j+nj && j+nj <= jhi_loop &&
                    edge_offset <= ithread_x && ithread_x < tilesize_x - edge_offset &&
                    edge_offset <= ithread_y+nj && ithread_y+nj < tilesize_y - edge_offset) {

                    res(i, j+nj, 0, 0) = residual1(i, j+nj, 0, ilo, jlo, ihi, jhi,
                                                phi_shared, rhs0_num[nj], acf_num[nj], facx, facy);
                    res(i, j+nj, 0, 1) = residual1(i, j+nj, 1, ilo, jlo, ihi, jhi,
                                                phi_shared, rhs1_num[nj], acf_num[nj], facx, facy);

                    phi(i, j+nj, 0, 0) = phi_shared(i, j+nj, 0, 0);
                    phi(i, j+nj, 0, 1) = phi_shared(i, j+nj, 0, 1);
                }
            }
        });
}

void gsrb_shared_st1_4_up_v2 (Box const& box, Array4<Real> const& phi,
                              Array4<Real const> const& rhs, Array4<Real const> const& acf,
                              Real dx, Real dy, Array4<Real> const& res)
{
    constexpr int tilesize = 64;
    constexpr int thread_tilesize = 32;
    constexpr int tilesize_array = tilesize + 2;
    constexpr int niter = 4;
    constexpr int edge_offset = niter;
    constexpr int final_tilesize = tilesize - 2*edge_offset;

    int const ilo = box.smallEnd(0);
    int const jlo = box.smallEnd(1);
    int const ihi = box.bigEnd(0);
    int const jhi = box.bigEnd(1);
    Real facx = Real(1.)/(dx*dx);
    Real facy = Real(1.)/(dy*dy);

    const Box loop_box = valid_domain_box(box);
    int const ilo_loop = loop_box.smallEnd(0);
    int const jlo_loop = loop_box.smallEnd(1);
    int const ihi_loop = loop_box.bigEnd(0);
    int const jhi_loop = loop_box.bigEnd(1);
    const int num_blocks_x = (loop_box.length(0) + final_tilesize - 1)/final_tilesize;
    const int num_blocks_y = (loop_box.length(1) + final_tilesize - 1)/final_tilesize;

    amrex::launch<thread_tilesize*thread_tilesize>(num_blocks_x*num_blocks_y, amrex::Gpu::gpuStream(),
        [=] AMREX_GPU_DEVICE() noexcept
        {
            __shared__ Real phi_ptr[tilesize_array*tilesize_array];

            const int iblock_y = blockIdx.x / num_blocks_x;
            const int iblock_x = blockIdx.x - iblock_y * num_blocks_x;

            const int tile_begin_x = iblock_x * final_tilesize - edge_offset - 1 + ilo_loop;
            const int tile_begin_y = iblock_y * final_tilesize - edge_offset - 1 + jlo_loop;

            const int tile_end_x = iblock_x * final_tilesize - edge_offset - 1 + ilo_loop + tilesize_array;
            const int tile_end_y = iblock_y * final_tilesize - edge_offset - 1 + jlo_loop + tilesize_array;

            Array4<Real> phi_shared(phi_ptr, {tile_begin_x,tile_begin_y,0},
                                             {tile_end_x,tile_end_y,1}, 1);

            const int ithread_y_0 = threadIdx.x / thread_tilesize;
            const int ithread_x_0 = threadIdx.x - ithread_y_0 * thread_tilesize;

            const int ithread_x[2] = {ithread_x_0, ithread_x_0 + thread_tilesize};
            const int ithread_y[2] = {ithread_y_0, ithread_y_0 + thread_tilesize};

            const int i_0 = iblock_x * final_tilesize + ithread_x_0 - edge_offset + ilo_loop;
            const int j_0 = iblock_y * final_tilesize + ithread_y_0 - edge_offset + jlo_loop;

            const int i[2] = {i_0, i_0 + thread_tilesize};
            const int j[2] = {j_0, j_0 + thread_tilesize};

            Real acf_num[2][2] = {0., 0., 0., 0.};

            for (int hj=0; hj<=1; ++hj) {
                for (int hi=0; hi<=1; ++hi) {
                    if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                        jlo_loop <= j[hj] && j[hj] <= jhi_loop) {
                        acf_num[hi][hj] = acf(i[hi], j[hj], 0);
                    }
                }
            }

            for (int n=0; n<=1; ++n) {

                for (int s = threadIdx.x; s < tilesize_array*tilesize_array; s+=blockDim.x) {
                    phi_ptr[s] = Real(0.);
                }

                __syncthreads();

                Real rhs_num[2][2] = {0., 0., 0., 0.};

                for (int hj=0; hj<=1; ++hj) {
                    for (int hi=0; hi<=1; ++hi) {
                        if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                            jlo_loop <= j[hj] && j[hj] <= jhi_loop) {
                            rhs_num[hi][hj] = rhs(i[hi], j[hj], 0, n);
                        }
                    }
                }

                for (int icolor=0; icolor<niter; ++icolor) {
                    for (int hj=0; hj<=1; ++hj) {
                        for (int hi=0; hi<=1; ++hi) {
                            if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                                jlo_loop <= j[hj] && j[hj] <= jhi_loop &&
                                (i[hi]+j[hj]+icolor)%2 == 0) {
                                gs1(i[hi], j[hj], 0, ilo, jlo, ihi, jhi, phi_shared,
                                    rhs_num[hi][hj], acf_num[hi][hj], facx, facy);
                            }
                        }
                    }
                    __syncthreads();
                }

                for (int hj=0; hj<=1; ++hj) {
                    for (int hi=0; hi<=1; ++hi) {
                        if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                            jlo_loop <= j[hj] && j[hj] <= jhi_loop &&
                            edge_offset <= ithread_x[hi] && ithread_x[hi] < tilesize - edge_offset &&
                            edge_offset <= ithread_y[hj] && ithread_y[hj] < tilesize - edge_offset) {

                            res(i[hi], j[hj], 0, n) = residual1(i[hi], j[hj], 0, ilo, jlo, ihi, jhi,
                                phi_shared, rhs_num[hi][hj], acf_num[hi][hj], facx, facy);

                            phi(i[hi], j[hj], 0, n) = phi_shared(i[hi], j[hj], 0, 0);
                        }
                    }
                }

                __syncthreads();
            }
        });
}


void gsrb_shared_st1_4_up_v4 (Box const& box, Array4<Real> const& phi,
                              Array4<Real const> const& rhs, Array4<Real const> const& acf,
                              Real dx, Real dy, Array4<Real> const& res)
{
    constexpr int tilesize = 64;
    constexpr int thread_tilesize = 32;
    constexpr int tilesize_array = tilesize + 2;
    constexpr int niter = 4;
    constexpr int edge_offset = niter;
    constexpr int final_tilesize = tilesize - 2*edge_offset;

    int const ilo = box.smallEnd(0);
    int const jlo = box.smallEnd(1);
    int const ihi = box.bigEnd(0);
    int const jhi = box.bigEnd(1);
    Real facx = Real(1.)/(dx*dx);
    Real facy = Real(1.)/(dy*dy);

    const Box loop_box = valid_domain_box(box);
    int const ilo_loop = loop_box.smallEnd(0);
    int const jlo_loop = loop_box.smallEnd(1);
    int const ihi_loop = loop_box.bigEnd(0);
    int const jhi_loop = loop_box.bigEnd(1);
    const int num_blocks_x = (loop_box.length(0) + final_tilesize - 1)/final_tilesize;
    const int num_blocks_y = (loop_box.length(1) + final_tilesize - 1)/final_tilesize;

    amrex::launch<thread_tilesize*thread_tilesize>(num_blocks_x*num_blocks_y, amrex::Gpu::gpuStream(),
        [=] AMREX_GPU_DEVICE() noexcept
        {
            __shared__ Real phi_ptr[tilesize_array*tilesize_array];

            const int iblock_y = blockIdx.x / num_blocks_x;
            const int iblock_x = blockIdx.x - iblock_y * num_blocks_x;

            const int tile_begin_x = iblock_x * final_tilesize - edge_offset - 1 + ilo_loop;
            const int tile_begin_y = iblock_y * final_tilesize - edge_offset - 1 + jlo_loop;

            const int tile_end_x = iblock_x * final_tilesize - edge_offset - 1 + ilo_loop + tilesize_array;
            const int tile_end_y = iblock_y * final_tilesize - edge_offset - 1 + jlo_loop + tilesize_array;

            Array4<Real> phi_shared(phi_ptr, {tile_begin_x,tile_begin_y,0},
                                             {tile_end_x,tile_end_y,1}, 1);

            const int ithread_y_0 = threadIdx.x / thread_tilesize;
            const int ithread_x_0 = threadIdx.x - ithread_y_0 * thread_tilesize;

            const int ithread_x[2] = {ithread_x_0, ithread_x_0 + thread_tilesize};
            const int ithread_y[2] = {2*ithread_y_0, 2*ithread_y_0+1};

            const int i_0 = iblock_x * final_tilesize - edge_offset + ilo_loop;
            const int j_0 = iblock_y * final_tilesize - edge_offset + jlo_loop;

            const int i[2] = {i_0 + ithread_x[0], i_0 + ithread_x[1]};
            const int j[2] = {j_0 + ithread_y[0], j_0 + ithread_y[1]};

            Real acf_num[2][2] = {0., 0., 0., 0.};

            for (int hj=0; hj<=1; ++hj) {
                for (int hi=0; hi<=1; ++hi) {
                    if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                        jlo_loop <= j[hj] && j[hj] <= jhi_loop) {
                        acf_num[hi][hj] = acf(i[hi], j[hj], 0);
                    }
                }
            }

            for (int n=0; n<=1; ++n) {

                for (int s = threadIdx.x; s < tilesize_array*tilesize_array; s+=blockDim.x) {
                    phi_ptr[s] = Real(0.);
                }

                __syncthreads();

                Real rhs_num[2][2] = {0., 0., 0., 0.};

                for (int hj=0; hj<=1; ++hj) {
                    for (int hi=0; hi<=1; ++hi) {
                        if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                            jlo_loop <= j[hj] && j[hj] <= jhi_loop) {
                            rhs_num[hi][hj] = rhs(i[hi], j[hj], 0, n);
                        }
                    }
                }

                for (int icolor=0; icolor<niter; ++icolor) {
                    for (int hi=0; hi<=1; ++hi) {
                        const int shift = (i[hi]+j[0]+icolor)%2;
                        const int j_loc = j[0] + shift;
                        Real rhs_loc = shift ? rhs_num[hi][1] : rhs_num[hi][0];
                        Real acf_loc = shift ? acf_num[hi][1] : acf_num[hi][0];

                        if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                            jlo_loop <= j_loc && j_loc <= jhi_loop) {
                            gs1(i[hi], j_loc, 0, ilo, jlo, ihi, jhi, phi_shared,
                                rhs_loc, acf_loc, facx, facy);
                        }
                    }
                    __syncthreads();
                }

                for (int hj=0; hj<=1; ++hj) {
                    for (int hi=0; hi<=1; ++hi) {
                        if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                            jlo_loop <= j[hj] && j[hj] <= jhi_loop &&
                            edge_offset <= ithread_x[hi] && ithread_x[hi] < tilesize - edge_offset &&
                            edge_offset <= ithread_y[hj] && ithread_y[hj] < tilesize - edge_offset) {

                            res(i[hi], j[hj], 0, n) = residual1(i[hi], j[hj], 0, ilo, jlo, ihi, jhi,
                                phi_shared, rhs_num[hi][hj], acf_num[hi][hj], facx, facy);

                            phi(i[hi], j[hj], 0, n) = phi_shared(i[hi], j[hj], 0, 0);
                        }
                    }
                }

                __syncthreads();
            }
        });
}


void gsrb_shared_st1_4_down_v2 (Box const& box, Array4<Real> const& phi_out,
                                Array4<Real const> const& rhs, Array4<Real const> const& acf,
                                Array4<Real const> const& phi_in, Real dx, Real dy)
{
    constexpr int tilesize = 64;
    constexpr int thread_tilesize = 32;
    constexpr int tilesize_array = tilesize + 2;
    constexpr int niter = 4;
    constexpr int edge_offset = niter - 1;
    constexpr int final_tilesize = tilesize - 2*edge_offset;

    int const ilo = box.smallEnd(0);
    int const jlo = box.smallEnd(1);
    int const ihi = box.bigEnd(0);
    int const jhi = box.bigEnd(1);
    Real facx = Real(1.)/(dx*dx);
    Real facy = Real(1.)/(dy*dy);

    const Box loop_box = valid_domain_box(box);
    int const ilo_loop = loop_box.smallEnd(0);
    int const jlo_loop = loop_box.smallEnd(1);
    int const ihi_loop = loop_box.bigEnd(0);
    int const jhi_loop = loop_box.bigEnd(1);
    const int num_blocks_x = (loop_box.length(0) + final_tilesize - 1)/final_tilesize;
    const int num_blocks_y = (loop_box.length(1) + final_tilesize - 1)/final_tilesize;

    amrex::launch<thread_tilesize*thread_tilesize>(num_blocks_x*num_blocks_y, amrex::Gpu::gpuStream(),
        [=] AMREX_GPU_DEVICE() noexcept
        {
            __shared__ Real phi_ptr[tilesize_array*tilesize_array];

            const int iblock_y = blockIdx.x / num_blocks_x;
            const int iblock_x = blockIdx.x - iblock_y * num_blocks_x;

            const int tile_begin_x = iblock_x * final_tilesize - edge_offset - 1 + ilo_loop;
            const int tile_begin_y = iblock_y * final_tilesize - edge_offset - 1 + jlo_loop;

            const int tile_end_x = iblock_x * final_tilesize - edge_offset - 1 + ilo_loop + tilesize_array;
            const int tile_end_y = iblock_y * final_tilesize - edge_offset - 1 + jlo_loop + tilesize_array;

            Array4<Real> phi_shared(phi_ptr, {tile_begin_x,tile_begin_y,0},
                                             {tile_end_x,tile_end_y,1}, 1);

            const int ithread_y_0 = threadIdx.x / thread_tilesize;
            const int ithread_x_0 = threadIdx.x - ithread_y_0 * thread_tilesize;

            const int ithread_x[2] = {ithread_x_0, ithread_x_0 + thread_tilesize};
            const int ithread_y[2] = {ithread_y_0, ithread_y_0 + thread_tilesize};

            const int i_0 = iblock_x * final_tilesize + ithread_x_0 - edge_offset + ilo_loop;
            const int j_0 = iblock_y * final_tilesize + ithread_y_0 - edge_offset + jlo_loop;

            const int i[2] = {i_0, i_0 + thread_tilesize};
            const int j[2] = {j_0, j_0 + thread_tilesize};

            Real acf_num[2][2] = {0., 0., 0., 0.};

            for (int hj=0; hj<=1; ++hj) {
                for (int hi=0; hi<=1; ++hi) {
                    if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                        jlo_loop <= j[hj] && j[hj] <= jhi_loop) {
                        acf_num[hi][hj] = acf(i[hi], j[hj], 0);
                    }
                }
            }

            for (int n=0; n<=1; ++n) {

                for (int s = threadIdx.x; s < tilesize_array*tilesize_array; s+=blockDim.x) {
                    int sy = s / tilesize_array;
                    int sx = s - sy * tilesize_array;
                    sx += tile_begin_x;
                    sy += tile_begin_y;
                    if (ilo_loop <= sx && sx <= ihi_loop &&
                        jlo_loop <= sy && sy <= jhi_loop) {
                        phi_shared(sx, sy, 0, 0) = phi_in(sx, sy, 0, n);
                    } else {
                        phi_shared(sx, sy, 0, 0) = Real(0.);
                    }
                }

                __syncthreads();

                Real rhs_num[2][2] = {0., 0., 0., 0.};

                for (int hj=0; hj<=1; ++hj) {
                    for (int hi=0; hi<=1; ++hi) {
                        if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                            jlo_loop <= j[hj] && j[hj] <= jhi_loop) {
                            rhs_num[hi][hj] = rhs(i[hi], j[hj], 0, n);
                        }
                    }
                }

                for (int icolor=0; icolor<niter; ++icolor) {
                    for (int hj=0; hj<=1; ++hj) {
                        for (int hi=0; hi<=1; ++hi) {
                            if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                                jlo_loop <= j[hj] && j[hj] <= jhi_loop &&
                                (i[hi]+j[hj]+icolor)%2 == 0) {
                                gs1(i[hi], j[hj], 0, ilo, jlo, ihi, jhi, phi_shared,
                                    rhs_num[hi][hj], acf_num[hi][hj], facx, facy);
                            }
                        }
                    }
                    __syncthreads();
                }

                for (int hj=0; hj<=1; ++hj) {
                    for (int hi=0; hi<=1; ++hi) {
                        if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                            jlo_loop <= j[hj] && j[hj] <= jhi_loop &&
                            edge_offset <= ithread_x[hi] && ithread_x[hi] < tilesize - edge_offset &&
                            edge_offset <= ithread_y[hj] && ithread_y[hj] < tilesize - edge_offset) {
                            phi_out(i[hi], j[hj], 0, n) = phi_shared(i[hi], j[hj], 0, 0);
                        }
                    }
                }

                __syncthreads();
            }
        });
}

void gsrb_shared_st1_4_down_v3 (Box const& box, Array4<Real> const& phi_out,
                                Array4<Real const> const& rhs, Array4<Real const> const& acf,
                                Array4<Real const> const& phi_in, Real dx, Real dy)
{
    constexpr int tilesize = 64;
    constexpr int thread_tilesize = 32;
    constexpr int tilesize_array = tilesize + 2;
    constexpr int niter = 4;
    constexpr int edge_offset = niter - 1;
    constexpr int final_tilesize = tilesize - 2*edge_offset;

    int const ilo = box.smallEnd(0);
    int const jlo = box.smallEnd(1);
    int const ihi = box.bigEnd(0);
    int const jhi = box.bigEnd(1);
    Real facx = Real(1.)/(dx*dx);
    Real facy = Real(1.)/(dy*dy);

    const Box loop_box = valid_domain_box(box);
    int const ilo_loop = loop_box.smallEnd(0);
    int const jlo_loop = loop_box.smallEnd(1);
    int const ihi_loop = loop_box.bigEnd(0);
    int const jhi_loop = loop_box.bigEnd(1);
    const int num_blocks_x = (loop_box.length(0) + final_tilesize - 1)/final_tilesize;
    const int num_blocks_y = (loop_box.length(1) + final_tilesize - 1)/final_tilesize;

    amrex::launch<thread_tilesize*thread_tilesize>(num_blocks_x*num_blocks_y, amrex::Gpu::gpuStream(),
        [=] AMREX_GPU_DEVICE() noexcept
        {
            __shared__ Real phi_ptr[tilesize_array*tilesize_array];

            const int iblock_y = blockIdx.x / num_blocks_x;
            const int iblock_x = blockIdx.x - iblock_y * num_blocks_x;

            const int tile_begin_x = iblock_x * final_tilesize - edge_offset - 1 + ilo_loop;
            const int tile_begin_y = iblock_y * final_tilesize - edge_offset - 1 + jlo_loop;

            const int tile_end_x = iblock_x * final_tilesize - edge_offset - 1 + ilo_loop + tilesize_array;
            const int tile_end_y = iblock_y * final_tilesize - edge_offset - 1 + jlo_loop + tilesize_array;

            Array4<Real> phi_shared(phi_ptr, {tile_begin_x,tile_begin_y,0},
                                             {tile_end_x,tile_end_y,1}, 1);

            const int ithread_y_0 = threadIdx.x / thread_tilesize;
            const int ithread_x_0 = threadIdx.x - ithread_y_0 * thread_tilesize;

            const int ithread_x[2] = {ithread_x_0, ithread_x_0 + thread_tilesize};
            const int ithread_y[2] = {ithread_y_0*2, ithread_y_0*2+1};

            const int i_0 = iblock_x * final_tilesize - edge_offset + ilo_loop;
            const int j_0 = iblock_y * final_tilesize - edge_offset + jlo_loop;

            const int i[2] = {i_0 + ithread_x[0], i_0 + ithread_x[1]};
            const int j[2] = {j_0 + ithread_y[0], j_0 + ithread_y[1]};

            Real acf_num[2][2] = {0., 0., 0., 0.};

            for (int hj=0; hj<=1; ++hj) {
                for (int hi=0; hi<=1; ++hi) {
                    if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                        jlo_loop <= j[hj] && j[hj] <= jhi_loop) {
                        acf_num[hi][hj] = acf(i[hi], j[hj], 0);
                    }
                }
            }

            for (int n=0; n<=1; ++n) {

                for (int s = threadIdx.x; s < tilesize_array*tilesize_array; s+=blockDim.x) {
                    int sy = s / tilesize_array;
                    int sx = s - sy * tilesize_array;
                    sx += tile_begin_x;
                    sy += tile_begin_y;
                    if (ilo_loop <= sx && sx <= ihi_loop &&
                        jlo_loop <= sy && sy <= jhi_loop) {
                        phi_shared(sx, sy, 0, 0) = phi_in(sx, sy, 0, n);
                    } else {
                        phi_shared(sx, sy, 0, 0) = Real(0.);
                    }
                }

                __syncthreads();

                Real rhs_num[2][2] = {0., 0., 0., 0.};

                for (int hj=0; hj<=1; ++hj) {
                    for (int hi=0; hi<=1; ++hi) {
                        if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                            jlo_loop <= j[hj] && j[hj] <= jhi_loop) {
                            rhs_num[hi][hj] = rhs(i[hi], j[hj], 0, n);
                        }
                    }
                }

                for (int icolor=0; icolor<niter; ++icolor) {

                    for (int hi=0; hi<=1; ++hi) {
                        const int shift = (i[hi]+j[0]+icolor)%2;
                        const int j_loc = j[0] + shift;
                        Real rhs_loc = shift ? rhs_num[hi][1] : rhs_num[hi][0];
                        Real acf_loc = shift ? acf_num[hi][1] : acf_num[hi][0];

                        if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                            jlo_loop <= j_loc && j_loc <= jhi_loop) {
                            gs1(i[hi], j_loc, 0, ilo, jlo, ihi, jhi, phi_shared,
                                rhs_loc, acf_loc, facx, facy);
                        }
                    }
                    __syncthreads();
                }

                for (int hj=0; hj<=1; ++hj) {
                    for (int hi=0; hi<=1; ++hi) {
                        if (ilo_loop <= i[hi] && i[hi] <= ihi_loop &&
                            jlo_loop <= j[hj] && j[hj] <= jhi_loop &&
                            edge_offset <= ithread_x[hi] && ithread_x[hi] < tilesize - edge_offset &&
                            edge_offset <= ithread_y[hj] && ithread_y[hj] < tilesize - edge_offset) {
                            phi_out(i[hi], j[hj], 0, n) = phi_shared(i[hi], j[hj], 0, 0);
                        }
                    }
                }

                __syncthreads();
            }
        });
}

void gsrb_shared_st1_4_down_v4 (Box const& box, Array4<Real> const& phi_out,
                                Array4<Real const> const& rhs, Array4<Real const> const& acf,
                                Array4<Real const> const& phi_in, Real dx, Real dy)
{
    constexpr int tilesize_x = 64;
    constexpr int tilesize_y = 32;
    constexpr int thread_tilesize = 32;
    constexpr int tilesize_array_x = tilesize_x + 2;
    constexpr int tilesize_array_y = tilesize_y + 2;
    constexpr int niter = 4;
    constexpr int edge_offset = niter - 1;
    constexpr int final_tilesize_x = tilesize_x - 2*edge_offset;
    constexpr int final_tilesize_y = tilesize_y - 2*edge_offset;

    int const ilo = box.smallEnd(0);
    int const jlo = box.smallEnd(1);
    int const ihi = box.bigEnd(0);
    int const jhi = box.bigEnd(1);
    Real facx = Real(1.)/(dx*dx);
    Real facy = Real(1.)/(dy*dy);

    const Box loop_box = valid_domain_box(box);
    int const ilo_loop = loop_box.smallEnd(0);
    int const jlo_loop = loop_box.smallEnd(1);
    int const ihi_loop = loop_box.bigEnd(0);
    int const jhi_loop = loop_box.bigEnd(1);
    const int num_blocks_x = (loop_box.length(0) + final_tilesize_x - 1)/final_tilesize_x;
    const int num_blocks_y = (loop_box.length(1) + final_tilesize_y - 1)/final_tilesize_y;

    amrex::launch<thread_tilesize*thread_tilesize>(num_blocks_x*num_blocks_y, amrex::Gpu::gpuStream(),
        [=] AMREX_GPU_DEVICE() noexcept
        {
            __shared__ Real phi_ptr[tilesize_array_x*tilesize_array_y*2];

            const int iblock_y = blockIdx.x / num_blocks_x;
            const int iblock_x = blockIdx.x - iblock_y * num_blocks_x;

            int ithread_y = threadIdx.x / tilesize_x;
            const int ithread_x = threadIdx.x - ithread_y * tilesize_x;
            ithread_y *= 2;

            const int i = iblock_x * final_tilesize_x + ithread_x - edge_offset + ilo_loop;
            const int j = iblock_y * final_tilesize_y + ithread_y - edge_offset + jlo_loop;

            const int tile_begin_x = iblock_x * final_tilesize_x - edge_offset - 1 + ilo_loop;
            const int tile_begin_y = iblock_y * final_tilesize_y - edge_offset - 1 + jlo_loop;

            const int tile_end_x = iblock_x * final_tilesize_x - edge_offset - 1 + ilo_loop + tilesize_array_x;
            const int tile_end_y = iblock_y * final_tilesize_y - edge_offset - 1 + jlo_loop + tilesize_array_y;

            Array4<Real> phi_shared(phi_ptr, {tile_begin_x,tile_begin_y,0},
                                             {tile_end_x,tile_end_y,1}, 2);

            for (int s = threadIdx.x; s < tilesize_array_x*tilesize_array_y; s+=blockDim.x) {
                int sy = s / tilesize_array_x;
                int sx = s - sy * tilesize_array_x;
                sx += tile_begin_x;
                sy += tile_begin_y;
                if (ilo_loop <= sx && sx <= ihi_loop &&
                    jlo_loop <= sy && sy <= jhi_loop) {
                    phi_shared(sx, sy, 0, 0) = phi_in(sx, sy, 0, 0);
                    phi_shared(sx, sy, 0, 1) = phi_in(sx, sy, 0, 1);
                } else {
                    phi_shared(sx, sy, 0, 0) = Real(0.);
                    phi_shared(sx, sy, 0, 1) = Real(0.);
                }
            }

            __syncthreads();

            Real rhs0_num[2] = {0., 0.};
            Real rhs1_num[2] = {0., 0.};
            Real acf_num[2] = {0., 0.};

            for (int nj=0; nj<=1; ++nj) {
                if (ilo_loop <= i && i <= ihi_loop &&
                    jlo_loop <= j+nj && j+nj <= jhi_loop) {
                    rhs0_num[nj] = rhs(i, j+nj, 0, 0);
                    rhs1_num[nj] = rhs(i, j+nj, 0, 1);
                    acf_num[nj] = acf(i, j+nj, 0);
                }
            }

            for (int icolor=0; icolor<niter; ++icolor) {
                const int shift = (i + j + icolor) % 2;
                const int j_loc = j + shift;
                const Real rhs0_loc = shift ? rhs0_num[1] : rhs0_num[0];
                const Real rhs1_loc = shift ? rhs1_num[1] : rhs1_num[0];
                const Real acf_loc = shift ? acf_num[1] : acf_num[0];
                if (ilo_loop <= i && i <= ihi_loop &&
                    jlo_loop <= j_loc && j_loc <= jhi_loop) {
                    gs1(i, j_loc, 0, ilo, jlo, ihi, jhi, phi_shared, rhs0_loc, acf_loc, facx, facy);
                    gs1(i, j_loc, 1, ilo, jlo, ihi, jhi, phi_shared, rhs1_loc, acf_loc, facx, facy);
                }
                __syncthreads();
            }

            for (int nj=0; nj<=1; ++nj) {
                if (ilo_loop <= i && i <= ihi_loop &&
                    jlo_loop <= j+nj && j+nj <= jhi_loop &&
                    edge_offset <= ithread_x && ithread_x < tilesize_x - edge_offset &&
                    edge_offset <= ithread_y+nj && ithread_y+nj < tilesize_y - edge_offset) {
                    phi_out(i, j+nj, 0, 0) = phi_shared(i, j+nj, 0, 0);
                    phi_out(i, j+nj, 0, 1) = phi_shared(i, j+nj, 0, 1);
                }
            }
        });
}


void gsrb_shared_st1_4_down (Box const& box, Array4<Real> const& phi_out,
                             Array4<Real const> const& rhs, Array4<Real const> const& acf,
                             Array4<Real const> const& phi_in, Real dx, Real dy)
{
    constexpr int tilesize = 32;
    constexpr int tilesize_array = tilesize + 2;
    constexpr int niter = 4;
    constexpr int edge_offset = niter - 1;
    constexpr int final_tilesize = tilesize - 2*edge_offset;

    int const ilo = box.smallEnd(0);
    int const jlo = box.smallEnd(1);
    int const ihi = box.bigEnd(0);
    int const jhi = box.bigEnd(1);
    Real facx = Real(1.)/(dx*dx);
    Real facy = Real(1.)/(dy*dy);

    const Box loop_box = valid_domain_box(box);
    int const ilo_loop = loop_box.smallEnd(0);
    int const jlo_loop = loop_box.smallEnd(1);
    int const ihi_loop = loop_box.bigEnd(0);
    int const jhi_loop = loop_box.bigEnd(1);
    const int num_blocks_x = (loop_box.length(0) + final_tilesize - 1)/final_tilesize;
    const int num_blocks_y = (loop_box.length(1) + final_tilesize - 1)/final_tilesize;

    amrex::launch<tilesize*tilesize>(num_blocks_x*num_blocks_y, amrex::Gpu::gpuStream(),
        [=] AMREX_GPU_DEVICE() noexcept
        {
            __shared__ Real phi_ptr[tilesize_array*tilesize_array*2];

            const int iblock_y = blockIdx.x / num_blocks_x;
            const int iblock_x = blockIdx.x - iblock_y * num_blocks_x;

            const int tile_begin_x = iblock_x * final_tilesize - niter + ilo_loop;
            const int tile_begin_y = iblock_y * final_tilesize - niter + jlo_loop;

            const int tile_end_x = iblock_x * final_tilesize - niter + ilo_loop + tilesize_array;
            const int tile_end_y = iblock_y * final_tilesize - niter + jlo_loop + tilesize_array;

            Array4<Real> phi_shared(phi_ptr, {tile_begin_x,tile_begin_y,0},
                                             {tile_end_x,tile_end_y,1}, 2);

            for (int s = threadIdx.x; s < tilesize_array*tilesize_array; s+=blockDim.x) {
                int sy = s / tilesize_array;
                int sx = s - sy * tilesize_array;
                sx += tile_begin_x;
                sy += tile_begin_y;
                if (ilo_loop <= sx && sx <= ihi_loop &&
                    jlo_loop <= sy && sy <= jhi_loop) {
                    phi_shared(sx, sy, 0, 0) = phi_in(sx, sy, 0, 0);
                    phi_shared(sx, sy, 0, 1) = phi_in(sx, sy, 0, 1);
                } else {
                    phi_shared(sx, sy, 0, 0) = Real(0.);
                    phi_shared(sx, sy, 0, 1) = Real(0.);
                }
            }

            __syncthreads();

            const int ithread_y = threadIdx.x / tilesize;
            const int ithread_x = threadIdx.x - ithread_y * tilesize;

            const int i = iblock_x * final_tilesize + ithread_x - edge_offset + ilo_loop;
            const int j = iblock_y * final_tilesize + ithread_y - edge_offset + jlo_loop;

            Real rhs0_num = Real(0.);
            Real rhs1_num = Real(0.);
            Real acf_num = Real(0.);

            if (ilo_loop <= i && i <= ihi_loop &&
                jlo_loop <= j && j <= jhi_loop) {
                rhs0_num = rhs(i, j, 0, 0);
                rhs1_num = rhs(i, j, 0, 1);
                acf_num = acf(i, j, 0);
            }

            for (int icolor=0; icolor<niter; ++icolor) {
                if (ilo_loop <= i && i <= ihi_loop &&
                    jlo_loop <= j && j <= jhi_loop &&
                    (i+j+icolor)%2 == 0) {
                    gs1(i, j, 0, ilo, jlo, ihi, jhi, phi_shared, rhs0_num, acf_num, facx, facy);
                    gs1(i, j, 1, ilo, jlo, ihi, jhi, phi_shared, rhs1_num, acf_num, facx, facy);
                }
                __syncthreads();
            }

            if (ilo_loop <= i && i <= ihi_loop &&
                jlo_loop <= j && j <= jhi_loop &&
                edge_offset <= ithread_x && ithread_x < tilesize - edge_offset &&
                edge_offset <= ithread_y && ithread_y < tilesize - edge_offset) {
                phi_out(i, j, 0, 0) = phi_shared(i, j, 0, 0);
                phi_out(i, j, 0, 1) = phi_shared(i, j, 0, 1);
            }
        });
}


template<bool zero_init, bool compute_residual, bool is_cell_centered>
void gsrb_shared_st1_4_uni_v1 (Box const& box,
                               Array4<Real> const& phi_out, Array4<Real const> const& rhs,
                               Array4<Real const> const& acf, Array4<Real> const& res,
                               Real dx, Real dy)
{
    constexpr int tilesize_x = 64;
    constexpr int tilesize_y = 32;
    constexpr int thread_tilesize = 32;
    constexpr int tilesize_array_x = tilesize_x + 2;
    constexpr int tilesize_array_y = tilesize_y + 2;
    constexpr int niter = 4;
    constexpr int edge_offset = niter - !compute_residual;
    constexpr int final_tilesize_x = tilesize_x - 2*edge_offset;
    constexpr int final_tilesize_y = tilesize_y - 2*edge_offset;
    static_assert(zero_init || !compute_residual);

    int const ilo = box.smallEnd(0);
    int const jlo = box.smallEnd(1);
    int const ihi = box.bigEnd(0);
    int const jhi = box.bigEnd(1);
    Real facx = Real(1.)/(dx*dx);
    Real facy = Real(1.)/(dy*dy);

    const Box loop_box = valid_domain_box(box);
    int const ilo_loop = loop_box.smallEnd(0);
    int const jlo_loop = loop_box.smallEnd(1);
    int const ihi_loop = loop_box.bigEnd(0);
    int const jhi_loop = loop_box.bigEnd(1);
    const int num_blocks_x = (loop_box.length(0) + final_tilesize_x - 1)/final_tilesize_x;
    const int num_blocks_y = (loop_box.length(1) + final_tilesize_y - 1)/final_tilesize_y;

    amrex::launch<thread_tilesize*thread_tilesize>(num_blocks_x*num_blocks_y, amrex::Gpu::gpuStream(),
        [=] AMREX_GPU_DEVICE() noexcept
        {
            __shared__ Real phi_ptr[tilesize_array_x*tilesize_array_y*2];

            const int iblock_y = blockIdx.x / num_blocks_x;
            const int iblock_x = blockIdx.x - iblock_y * num_blocks_x;

            const int tile_begin_x = iblock_x * final_tilesize_x - edge_offset - 1 + ilo_loop;
            const int tile_begin_y = iblock_y * final_tilesize_y - edge_offset - 1 + jlo_loop;

            const int tile_end_x = iblock_x * final_tilesize_x - edge_offset - 1 + ilo_loop + tilesize_array_x;
            const int tile_end_y = iblock_y * final_tilesize_y - edge_offset - 1 + jlo_loop + tilesize_array_y;

            Array4<Real> phi_shared(phi_ptr, {tile_begin_x,tile_begin_y,0},
                                             {tile_end_x,tile_end_y,1}, 2);

            if (zero_init) {
                for (int s = threadIdx.x; s < tilesize_array_x*tilesize_array_y*2; s+=blockDim.x) {
                    phi_ptr[s] = Real(0.);
                }
            } else {
                for (int s = threadIdx.x; s < tilesize_array_x*tilesize_array_y; s+=blockDim.x) {
                    int sy = s / tilesize_array_x;
                    int sx = s - sy * tilesize_array_x;
                    sx += tile_begin_x;
                    sy += tile_begin_y;
                    if (ilo_loop <= sx && sx <= ihi_loop &&
                        jlo_loop <= sy && sy <= jhi_loop) {
                        phi_shared(sx, sy, 0, 0) = res(sx, sy, 0, 0);
                        phi_shared(sx, sy, 0, 1) = res(sx, sy, 0, 1);
                    } else {
                        phi_shared(sx, sy, 0, 0) = Real(0.);
                        phi_shared(sx, sy, 0, 1) = Real(0.);
                    }
                }
            }

            int ithread_y = threadIdx.x / tilesize_x;
            const int ithread_x = threadIdx.x - ithread_y * tilesize_x;
            ithread_y *= 2;

            const int i = iblock_x * final_tilesize_x + ithread_x - edge_offset + ilo_loop;
            const int j = iblock_y * final_tilesize_y + ithread_y - edge_offset + jlo_loop;

            Real rhs0_num[2] = {0., 0.};
            Real rhs1_num[2] = {0., 0.};
            Real acf_num[2] = {0., 0.};

            for (int nj=0; nj<=1; ++nj) {
                if (ilo_loop <= i && i <= ihi_loop &&
                    jlo_loop <= j+nj && j+nj <= jhi_loop) {
                    rhs0_num[nj] = rhs(i, j+nj, 0, 0);
                    rhs1_num[nj] = rhs(i, j+nj, 0, 1);
                    acf_num[nj] = acf(i, j+nj, 0);
                }
            }

            __syncthreads();

            for (int icolor=0; icolor<niter; ++icolor) {
                const int shift = (i + j + icolor) % 2;
                const int j_loc = j + shift;
                const Real rhs0_loc = shift ? rhs0_num[1] : rhs0_num[0];
                const Real rhs1_loc = shift ? rhs1_num[1] : rhs1_num[0];
                const Real acf_loc = shift ? acf_num[1] : acf_num[0];
                if (ilo_loop <= i && i <= ihi_loop &&
                    jlo_loop <= j_loc && j_loc <= jhi_loop) {
                    gs1<is_cell_centered>(i, j_loc, 0, ilo, jlo, ihi, jhi, phi_shared,
                                          rhs0_loc, acf_loc, facx, facy);
                    gs1<is_cell_centered>(i, j_loc, 1, ilo, jlo, ihi, jhi, phi_shared,
                                          rhs1_loc, acf_loc, facx, facy);
                }
                __syncthreads();
            }

            for (int nj=0; nj<=1; ++nj) {
                if (ilo_loop <= i && i <= ihi_loop &&
                    jlo_loop <= j+nj && j+nj <= jhi_loop &&
                    edge_offset <= ithread_x && ithread_x < tilesize_x - edge_offset &&
                    edge_offset <= ithread_y+nj && ithread_y+nj < tilesize_y - edge_offset) {

                    if (compute_residual) {
                        res(i, j+nj, 0, 0) = residual1(
                                                i, j+nj, 0, ilo, jlo, ihi, jhi, phi_shared,
                                                rhs0_num[nj], acf_num[nj], facx, facy);
                        res(i, j+nj, 0, 1) = residual1(
                                                i, j+nj, 1, ilo, jlo, ihi, jhi, phi_shared,
                                                rhs1_num[nj], acf_num[nj], facx, facy);
                    }

                    phi_out(i, j+nj, 0, 0) = phi_shared(i, j+nj, 0, 0);
                    phi_out(i, j+nj, 0, 1) = phi_shared(i, j+nj, 0, 1);
                }
            }
        });
}


void restriction (Box const& box, Array4<Real> const& crse, Array4<Real const> const& fine)
{
    if (box.cellCentered()) {
        hpmg::ParallelFor(box, 2, [=] AMREX_GPU_DEVICE (int i, int j, int, int n) noexcept
        {
            restrict_cc(i,j,n,crse,fine);
        });
    } else {
        hpmg::ParallelFor(valid_domain_box(box), 2,
        [=] AMREX_GPU_DEVICE (int i, int j, int, int n) noexcept
        {
            restrict_nd(i,j,n,crse,fine);
        });
    }
}

void interpolation (Box const& box, Array4<Real> const& fine, Array4<Real const> const& crse)
{
    if (box.cellCentered()) {
        hpmg::ParallelFor(box, 2, [=] AMREX_GPU_DEVICE (int i, int j, int, int n) noexcept
        {
            interpadd_cc(i,j,n,fine,crse);
        });
    } else {
        hpmg::ParallelFor(valid_domain_box(box), 2,
        [=] AMREX_GPU_DEVICE (int i, int j, int, int n) noexcept
        {
            interpadd_nd(i,j,n,fine,crse);
        });
    }
}

void interpolation_outofplace (Box const& box, Array4<Real const> const& fine_in,
                               Array4<Real const> const& crse, Array4<Real> const& fine_out)
{
    if (box.cellCentered()) {
        hpmg::ParallelFor(box, 2, [=] AMREX_GPU_DEVICE (int i, int j, int, int n) noexcept
        {
            interpcpy_cc(i,j,n,fine_in,crse,fine_out);
        });
    } else {
        hpmg::ParallelFor(valid_domain_box(box), 2,
        [=] AMREX_GPU_DEVICE (int i, int j, int, int n) noexcept
        {
            interpcpy_nd(i,j,n,fine_in,crse,fine_out);
        });
    }
}

#if defined(AMREX_USE_GPU)

#if defined(AMREX_USE_DPCPP)
#define HPMG_SYNCTHREADS item.barrier(sycl::access::fence_space::global_and_local)
#else
#define HPMG_SYNCTHREADS __syncthreads()
#endif

template <int NS, typename FGS, typename FRES>
void bottomsolve_gpu (Real dx0, Real dy0, Array4<Real> const* acf,
                      Array4<Real> const* res, Array4<Real> const* cor,
                      Array4<Real> const* rescor, int nlevs, int corner_offset,
                      FGS&& fgs, FRES&& fres)
{
    static_assert(n_cell_single*n_cell_single <= 1024, "n_cell_single is too big");
#if defined(AMREX_USE_DPCPP)
    amrex::launch(1, 1024, Gpu::gpuStream(),
    [=] (sycl::nd_item<1> const& item) noexcept
#else
    amrex::launch_global<1024><<<1, 1024, 0, Gpu::gpuStream()>>>(
    [=] AMREX_GPU_DEVICE () noexcept
#endif
    {
        Real facx = Real(1.)/(dx0*dx0);
        Real facy = Real(1.)/(dy0*dy0);
        int lenx = cor[0].end.x - cor[0].begin.x - 2*corner_offset;
        int leny = cor[0].end.y - cor[0].begin.y - 2*corner_offset;
        int ncells = lenx*leny;
#if defined(AMREX_USE_DPCPP)
        const int icell = item.get_local_linear_id();
#else
        const int icell = threadIdx.x;
#endif
        int j = icell /   lenx;
        int i = icell - j*lenx;
        j += cor[0].begin.y + corner_offset;
        i += cor[0].begin.x + corner_offset;

        for (int ilev = 0; ilev < nlevs-1; ++ilev) {
            if (icell < ncells) {
                cor[ilev](i,j,0,0) = Real(0.);
                cor[ilev](i,j,0,1) = Real(0.);
            }
            HPMG_SYNCTHREADS;

            for (int is = 0; is < 4; ++is) {
                if (icell < ncells) {
                    if ((i+j+is)%2 == 0) {
                        fgs(i, j,
                            cor[ilev].begin.x, cor[ilev].begin.y,
                            cor[ilev].end.x-1, cor[ilev].end.y-1,
                            cor[ilev],
                            res[ilev](i,j,0,0),
                            res[ilev](i,j,0,1),
                            acf[ilev], facx, facy);
                    }
                }
                HPMG_SYNCTHREADS;
            }

            if (icell < ncells) {
                fres(i, j,
                     rescor[ilev](i,j,0,0),
                     rescor[ilev](i,j,0,1),
                     cor[ilev].begin.x, cor[ilev].begin.y,
                     cor[ilev].end.x-1, cor[ilev].end.y-1,
                     cor[ilev],
                     res[ilev](i,j,0,0),
                     res[ilev](i,j,0,1),
                     acf[ilev], facx, facy);
            }
            HPMG_SYNCTHREADS;

            lenx = cor[ilev+1].end.x - cor[ilev+1].begin.x - 2*corner_offset;
            leny = cor[ilev+1].end.y - cor[ilev+1].begin.y - 2*corner_offset;
            ncells = lenx*leny;
            if (icell < ncells) {
                j = icell /   lenx;
                i = icell - j*lenx;
                j += cor[ilev+1].begin.y + corner_offset;
                i += cor[ilev+1].begin.x + corner_offset;
                if (corner_offset == 0) {
                    restrict_cc(i,j,0,res[ilev+1],rescor[ilev]);
                    restrict_cc(i,j,1,res[ilev+1],rescor[ilev]);
                } else {
                    restrict_nd(i,j,0,res[ilev+1],rescor[ilev]);
                    restrict_nd(i,j,1,res[ilev+1],rescor[ilev]);
                }
            }
            HPMG_SYNCTHREADS;

            facx *= Real(0.25);
            facy *= Real(0.25);
        }

        // bottom
        {
            const int ilev = nlevs-1;
            if (icell < ncells) {
                cor[ilev](i,j,0,0) = Real(0.);
                cor[ilev](i,j,0,1) = Real(0.);
            }
            HPMG_SYNCTHREADS;

            for (int is = 0; is < NS; ++is) {
                if (icell < ncells) {
                    if ((i+j+is)%2 == 0) {
                        fgs(i, j,
                            cor[ilev].begin.x, cor[ilev].begin.y,
                            cor[ilev].end.x-1, cor[ilev].end.y-1,
                            cor[ilev],
                            res[ilev](i,j,0,0),
                            res[ilev](i,j,0,1),
                            acf[ilev], facx, facy);
                    }
                }
                HPMG_SYNCTHREADS;
            }
        }

        for (int ilev = nlevs-2; ilev >=0; --ilev) {
            lenx = cor[ilev].end.x - cor[ilev].begin.x - 2*corner_offset;
            leny = cor[ilev].end.y - cor[ilev].begin.y - 2*corner_offset;
            ncells = lenx*leny;
            facx *= Real(4.);
            facy *= Real(4.);

            if (icell < ncells) {
                j = icell /   lenx;
                i = icell - j*lenx;
                j += cor[ilev].begin.y + corner_offset;
                i += cor[ilev].begin.x + corner_offset;
                if (corner_offset == 0) {
                    interpadd_cc(i, j, 0, cor[ilev], cor[ilev+1]);
                    interpadd_cc(i, j, 1, cor[ilev], cor[ilev+1]);
                } else {
                    interpadd_nd(i, j, 0, cor[ilev], cor[ilev+1]);
                    interpadd_nd(i, j, 1, cor[ilev], cor[ilev+1]);
                }
            }

            for (int is = 0; is < 4; ++is) {
                HPMG_SYNCTHREADS;
                if (icell < ncells) {
                    if ((i+j+is)%2 == 0) {
                        fgs(i, j,
                            cor[ilev].begin.x, cor[ilev].begin.y,
                            cor[ilev].end.x-1, cor[ilev].end.y-1,
                            cor[ilev],
                            res[ilev](i,j,0,0),
                            res[ilev](i,j,0,1),
                            acf[ilev], facx, facy);
                    }
                }
            }
        }
    });
}

#endif // AMREX_USE_GPU

} // namespace {}

MultiGrid::MultiGrid (Real dx, Real dy, Box a_domain)
    : m_dx(dx), m_dy(dy)
{
    IntVect const a_domain_len = a_domain.length();

    AMREX_ALWAYS_ASSERT(a_domain_len[2] == 1 && a_domain.cellCentered() &&
                        a_domain_len[0]%2 == a_domain_len[1]%2);

    IndexType const index_type = (a_domain_len[0]%2 == 0) ?
        IndexType::TheCellType() : IndexType(IntVect(1,1,0));
    m_domain.push_back(amrex::makeSlab(Box{{0,0,0}, a_domain_len-1, index_type}, 2, 0));
    if (!index_type.cellCentered()) {
        m_domain[0].growHi(0,2).growHi(1,2);
    }
    IntVect const min_width = index_type.cellCentered() ? IntVect(2,2,1) : IntVect(4,4,1);
    for (int i = 0; i < 30; ++i) {
        if (m_domain.back().coarsenable(IntVect(2,2,1), min_width)) {
            m_domain.push_back(amrex::coarsen(m_domain.back(),IntVect(2,2,1)));
        } else {
            break;
        }
    }
    m_max_level = m_domain.size()-1;
#if defined(AMREX_USE_GPU)
    auto r = std::find_if(std::begin(m_domain), std::end(m_domain),
                          [=] (Box const& b) -> bool
                              { return b.volume() <= n_cell_single*n_cell_single; });
    m_single_block_level_begin = std::distance(std::begin(m_domain), r);
    m_single_block_level_begin = std::max(1, m_single_block_level_begin);
#else
    m_single_block_level_begin = m_max_level;
#endif

    m_num_mg_levels = m_max_level+1;
    m_num_single_block_levels = m_num_mg_levels - m_single_block_level_begin;

    if (m_num_single_block_levels > 0) {
        m_h_array4.reserve(nfabvs*m_num_single_block_levels);
    }

    m_acf.reserve(m_num_mg_levels);
    for (int ilev = 0; ilev < m_num_mg_levels; ++ilev) {
        m_acf.emplace_back(m_domain[ilev], 2);
        if (ilev >= m_single_block_level_begin) {
            m_h_array4.push_back(m_acf[ilev].array());
        }
    }

    m_res.reserve(m_num_mg_levels);
    for (int ilev = 0; ilev < m_num_mg_levels; ++ilev) {
        m_res.emplace_back(m_domain[ilev], 2);
        if (!index_type.cellCentered()) {
            m_res[ilev].template setVal<RunOn::Device>(0);
        }
        if (ilev >= m_single_block_level_begin) {
            m_h_array4.push_back(m_res[ilev].array());
        }
    }

    m_cor.reserve(m_num_mg_levels);
    for (int ilev = 0; ilev < m_num_mg_levels; ++ilev) {
        m_cor.emplace_back(m_domain[ilev], 2);
        if (ilev >= m_single_block_level_begin) {
            if (!index_type.cellCentered()) {
                m_cor[ilev].template setVal<RunOn::Device>(0);
            }
            m_h_array4.push_back(m_cor[ilev].array());
        }
    }

    m_rescor.reserve(m_num_mg_levels);
    for (int ilev = 0; ilev < m_num_mg_levels; ++ilev) {
        m_rescor.emplace_back(m_domain[ilev], 2);
        if (!index_type.cellCentered()) {
            m_rescor[ilev].template setVal<RunOn::Device>(0);
        }
        if (ilev >= m_single_block_level_begin) {
            m_h_array4.push_back(m_rescor[ilev].array());
        }
    }

    if (!m_h_array4.empty()) {
        m_d_array4.resize(m_h_array4.size());
        Gpu::copyAsync(Gpu::hostToDevice, m_h_array4.begin(), m_h_array4.end(),
                       m_d_array4.begin());
        m_acf_a = m_d_array4.data();
        m_res_a = m_acf_a + m_num_single_block_levels;
        m_cor_a = m_res_a + m_num_single_block_levels;
        m_rescor_a = m_cor_a + m_num_single_block_levels;
    }
}

void
MultiGrid::solve1 (FArrayBox& a_sol, FArrayBox const& a_rhs, FArrayBox const& a_acf,
                   Real const tol_rel, Real const tol_abs, int const nummaxiter,
                   int const verbose)
{
    HIPACE_PROFILE("hpmg::MultiGrid::solve1()");
    m_system_type = 1;

    FArrayBox afab(center_box(a_acf.box(), m_domain.front()), 1, a_acf.dataPtr());

    auto const& array_m_acf = m_acf[0].array();
    auto const& array_a_acf = afab.const_array();
    hpmg::ParallelFor(m_acf[0].box(),
        [=] AMREX_GPU_DEVICE (int i, int j, int) noexcept
        {
            array_m_acf(i,j,0) = array_a_acf(i,j,0);
        });

    average_down_acoef();

    solve_doit(a_sol, a_rhs, tol_rel, tol_abs, nummaxiter, verbose);
}

void
MultiGrid::solve2 (amrex::FArrayBox& sol, amrex::FArrayBox const& rhs,
                   amrex::Real const acoef_real, amrex::Real const acoef_imag,
                   amrex::Real const tol_rel, amrex::Real const tol_abs,
                   int const nummaxiter, int const verbose)
{
    HIPACE_PROFILE("hpmg::MultiGrid::solve2()");
    m_system_type = 2;

    auto const& array_m_acf = m_acf[0].array();

    hpmg::ParallelFor(m_acf[0].box(),
        [=] AMREX_GPU_DEVICE (int i, int j, int) noexcept
        {
            array_m_acf(i,j,0,0) = acoef_real;
            array_m_acf(i,j,0,1) = acoef_imag;
        });

    average_down_acoef();

    solve_doit(sol, rhs, tol_rel, tol_abs, nummaxiter, verbose);
}

void
MultiGrid::solve2 (amrex::FArrayBox& sol, amrex::FArrayBox const& rhs,
                   amrex::Real const acoef_real, amrex::FArrayBox const& acoef_imag,
                   amrex::Real const tol_rel, amrex::Real const tol_abs,
                   int const nummaxiter, int const verbose)
{
    HIPACE_PROFILE("hpmg::MultiGrid::solve2()");
    m_system_type = 2;

    auto const& array_m_acf = m_acf[0].array();

    amrex::FArrayBox ifab(center_box(acoef_imag.box(), m_domain.front()), 1, acoef_imag.dataPtr());
    auto const& ai = ifab.const_array();
    hpmg::ParallelFor(m_acf[0].box(),
        [=] AMREX_GPU_DEVICE (int i, int j, int) noexcept
        {
            array_m_acf(i,j,0,0) = acoef_real;
            array_m_acf(i,j,0,1) = ai(i,j,0);
        });

    average_down_acoef();

    solve_doit(sol, rhs, tol_rel, tol_abs, nummaxiter, verbose);
}

void
MultiGrid::solve2 (amrex::FArrayBox& sol, amrex::FArrayBox const& rhs,
                   amrex::FArrayBox const& acoef_real, amrex::Real const acoef_imag,
                   amrex::Real const tol_rel, amrex::Real const tol_abs,
                   int const nummaxiter, int const verbose)
{
    HIPACE_PROFILE("hpmg::MultiGrid::solve2()");
    m_system_type = 2;

    auto const& array_m_acf = m_acf[0].array();

    amrex::FArrayBox rfab(center_box(acoef_real.box(), m_domain.front()), 1, acoef_real.dataPtr());
    auto const& ar = rfab.const_array();
    hpmg::ParallelFor(m_acf[0].box(),
        [=] AMREX_GPU_DEVICE (int i, int j, int) noexcept
        {
            array_m_acf(i,j,0,0) = ar(i,j,0);
            array_m_acf(i,j,0,1) = acoef_imag;
        });

    average_down_acoef();

    solve_doit(sol, rhs, tol_rel, tol_abs, nummaxiter, verbose);
}

void
MultiGrid::solve2 (amrex::FArrayBox& sol, amrex::FArrayBox const& rhs,
                   amrex::FArrayBox const& acoef_real, amrex::FArrayBox const& acoef_imag,
                   amrex::Real const tol_rel, amrex::Real const tol_abs,
                   int const nummaxiter, int const verbose)
{
    HIPACE_PROFILE("hpmg::MultiGrid::solve2()");
    m_system_type = 2;

    auto const& array_m_acf = m_acf[0].array();

    amrex::FArrayBox rfab(center_box(acoef_real.box(), m_domain.front()), 1, acoef_real.dataPtr());
    amrex::FArrayBox ifab(center_box(acoef_imag.box(), m_domain.front()), 1, acoef_imag.dataPtr());
    auto const& ar = rfab.const_array();
    auto const& ai = ifab.const_array();
    hpmg::ParallelFor(m_acf[0].box(),
        [=] AMREX_GPU_DEVICE (int i, int j, int) noexcept
        {
            array_m_acf(i,j,0,0) = ar(i,j,0);
            array_m_acf(i,j,0,1) = ai(i,j,0);
        });

    average_down_acoef();

    solve_doit(sol, rhs, tol_rel, tol_abs, nummaxiter, verbose);
}

void
MultiGrid::solve_doit (FArrayBox& a_sol, FArrayBox const& a_rhs,
                       Real const tol_rel, Real const tol_abs, int const nummaxiter,
                       int const verbose)
{
    AMREX_ALWAYS_ASSERT(a_sol.nComp() >= 2 && a_rhs.nComp() >= 2);

    m_sol = FArrayBox(center_box(a_sol.box(), m_domain.front()), 2, a_sol.dataPtr());
    m_rhs = FArrayBox(center_box(a_rhs.box(), m_domain.front()), 2, a_rhs.dataPtr());

    compute_residual(m_domain[0], m_res[0].array(), m_sol.array(),
                     m_rhs.const_array(), m_acf[0].const_array(), m_dx, m_dy,
                     m_system_type);

    Real resnorm0, rhsnorm0;
    {
        ReduceOps<ReduceOpMax,ReduceOpMax> reduce_op;
        ReduceData<Real,Real> reduce_data(reduce_op);
        using ReduceTuple = typename decltype(reduce_data)::Type;
        const auto& array_res = m_res[0].const_array();
        const auto& array_rhs = m_rhs.const_array();
        reduce_op.eval(valid_domain_box(m_domain[0]), 2, reduce_data,
            [=] AMREX_GPU_DEVICE (int i, int j, int, int n) noexcept -> ReduceTuple
            {
                return {std::abs(array_res(i,j,0,n)), std::abs(array_rhs(i,j,0,n))};
            });

        auto hv = reduce_data.value(reduce_op);
        resnorm0 = amrex::get<0>(hv);
        rhsnorm0 = amrex::get<1>(hv);
    }
    if (verbose >= 1) {
        amrex::Print() << "hpmg: Initial rhs               = " << rhsnorm0 << "\n"
                       << "hpmg: Initial residual (resid0) = " << resnorm0 << "\n";
    }

    Real max_norm;
    std::string norm_name;
    if (rhsnorm0 >= resnorm0) {
        norm_name = "bnorm";
        max_norm = rhsnorm0;
    } else {
        norm_name = "resid0";
        max_norm = resnorm0;
    }
    const Real res_target = std::max(tol_abs, std::max(tol_rel,Real(1.e-16))*max_norm);

    if (resnorm0 <= res_target) {
        if (verbose >= 1) {
            amrex::Print() << "hpmg: No iterations needed\n";
        }
    } else {
        Real norminf = 0.;
        bool converged = true;

        for (int iter = 0; iter < nummaxiter; ++iter) {

            converged = false;

            vcycle();

            compute_residual(m_domain[0], m_res[0].array(), m_sol.array(),
                             m_rhs.const_array(), m_acf[0].const_array(), m_dx, m_dy,
                             m_system_type);

            Real const* pres0 = m_res[0].dataPtr();
            norminf = Reduce::Max<Real>(m_domain[0].numPts()*2,
                                        [=] AMREX_GPU_DEVICE (Long i) -> Real
                                        {
                                            return std::abs(pres0[i]);
                                        });
            if (verbose >= 2) {
                amrex::Print() << "hpmg: Iteration " << std::setw(3) << iter+1 << " resid/"
                               << norm_name << " = " << norminf/max_norm << "\n";
            }

            converged = (norminf <= res_target);
            if (converged) {
                if (verbose >= 1) {
                    amrex::Print() << "hpmg: Final Iter. " << iter+1
                                   << " resid, resid/" << norm_name << " = "
                                   << norminf << ", " << norminf/max_norm << "\n";
                }
                break;
            } else if (norminf > Real(1.e20)*max_norm) {
                if (verbose > 0) {
                    amrex::Print() << "hpmg: Failing to converge after " << iter+1 << " iterations."
                                   << " resid, resid/" << norm_name << " = "
                                   << norminf << ", " << norminf/max_norm << "\n";
                  }
                  amrex::Abort("hpmg failing so lets stop here");
            }
        }

        if (!converged) {
            if (verbose > 0) {
                amrex::Print() << "hpmg: Failed to converge after " << nummaxiter << " iterations."
                               << " resid, resid/" << norm_name << " = "
                               << norminf << ", " << norminf/max_norm << "\n";
            }
            amrex::Abort("hpmg failed");
        }
    }
}

void
MultiGrid::vcycle ()
{
#if defined(AMREX_USE_CUDA)
    const int igraph = m_system_type-1;
    bool& graph_created = m_cuda_graph_vcycle_created[igraph];
    cudaGraph_t& graph = m_cuda_graph_vcycle[igraph];
    cudaGraphExec_t& graph_exe = m_cuda_graph_exe_vcycle[igraph];
    if (!graph_created) {
    cudaStreamBeginCapture(Gpu::gpuStream(), cudaStreamCaptureModeGlobal);
#endif

    for (int ilev = 0; ilev < m_single_block_level_begin; ++ilev) {
        //Real * pcor = m_cor[ilev].dataPtr();
        //hpmg::ParallelFor(m_domain[ilev].numPts()*2,
        //                  [=] AMREX_GPU_DEVICE (Long i) noexcept { pcor[i] = Real(0.); });

        Real fac = static_cast<Real>(1 << ilev);
        Real dx = m_dx * fac;
        Real dy = m_dy * fac;
        /*for (int is = 0; is < 4; ++is) {
            gsrb(is, m_domain[ilev], m_cor[ilev].array(),
                 m_res[ilev].const_array(), m_acf[ilev].const_array(), dx, dy,
                 m_system_type);
        }*/

        //gsrb_shared_st1_4_up_v5(m_domain[ilev], m_cor[ilev].array(),
        //                        m_res[ilev].const_array(), m_acf[ilev].const_array(), dx, dy,
        //                        m_rescor[ilev].array());

        if (m_domain[ilev].cellCentered()) {
            gsrb_shared_st1_4_uni_v1<true, true, true>(
                m_domain[ilev], m_cor[ilev].array(), m_res[ilev].const_array(),
                m_acf[ilev].const_array(), m_rescor[ilev].array(), dx, dy);

        } else {
            gsrb_shared_st1_4_uni_v1<true, true, false>(
                m_domain[ilev], m_cor[ilev].array(), m_res[ilev].const_array(),
                m_acf[ilev].const_array(), m_rescor[ilev].array(), dx, dy);
        }


        // rescor = res - L(cor)
        //compute_residual(m_domain[ilev], m_rescor[ilev].array(), m_cor[ilev].array(),
        //                 m_res[ilev].const_array(), m_acf[ilev].const_array(), dx, dy,
        //                 m_system_type);

        // res[ilev+1] = R(rescor[ilev])
        restriction(m_domain[ilev+1], m_res[ilev+1].array(), m_rescor[ilev].const_array());
    }

    bottomsolve();

    for (int ilev = m_single_block_level_begin-1; ilev >= 0; --ilev) {
        // cor[ilev] += I(cor[ilev+1])
        interpolation_outofplace(m_domain[ilev], m_cor[ilev].const_array(),
                                 m_cor[ilev+1].const_array(), m_rescor[ilev].array());

        Real fac = static_cast<Real>(1 << ilev);
        Real dx = m_dx * fac;
        Real dy = m_dy * fac;
        /*for (int is = 0; is < 4; ++is) {
            gsrb(is, m_domain[ilev], m_cor[ilev].array(),
                 m_res[ilev].const_array(), m_acf[ilev].const_array(), dx, dy,
                 m_system_type);
        }*/

        //gsrb_shared_st1_4_down_v4(m_domain[ilev], m_cor[ilev].array(),
        //                          m_res[ilev].const_array(), m_acf[ilev].const_array(),
        //                          m_rescor[ilev].const_array(), dx, dy);

        if (m_domain[ilev].cellCentered()) {
            gsrb_shared_st1_4_uni_v1<false, false, true>(
                m_domain[ilev], m_cor[ilev].array(), m_res[ilev].const_array(),
                m_acf[ilev].const_array(), m_rescor[ilev].array(), dx, dy);
        } else {
            gsrb_shared_st1_4_uni_v1<false, false, false>(
                m_domain[ilev], m_cor[ilev].array(), m_res[ilev].const_array(),
                m_acf[ilev].const_array(), m_rescor[ilev].array(), dx, dy);
        }

    }

#if defined(AMREX_USE_CUDA)
    cudaStreamEndCapture(Gpu::gpuStream(), &graph);
    cudaGraphInstantiate(&graph_exe, graph, NULL, NULL, 0);
    graph_created = true;
    }
    cudaGraphLaunch(graph_exe, Gpu::gpuStream());
#endif

    auto const& sol = m_sol.array();
    auto const& cor = m_cor[0].const_array();
    hpmg::ParallelFor(valid_domain_box(m_domain[0]), 2,
    [=] AMREX_GPU_DEVICE (int i, int j, int, int n) noexcept
    {
        sol(i,j,0,n) += cor(i,j,0,n);
    });
}

void
MultiGrid::bottomsolve ()
{
    constexpr int nsweeps = 16;
    Real fac = static_cast<Real>(1 << m_single_block_level_begin);
    Real dx0 = m_dx * fac;
    Real dy0 = m_dy * fac;
#if defined(AMREX_USE_GPU)
    Array4<amrex::Real> const* acf = m_acf_a;
    Array4<amrex::Real> const* res = m_res_a;
    Array4<amrex::Real> const* cor = m_cor_a;
    Array4<amrex::Real> const* rescor = m_rescor_a;
    int nlevs = m_num_single_block_levels;
    int const corner_offset = m_domain[0].cellCentered() ? 0 : 1;

    if (m_system_type == 1) {
        bottomsolve_gpu<nsweeps>(dx0, dy0, acf, res, cor, rescor, nlevs, corner_offset,
            [=] AMREX_GPU_DEVICE (int i, int j, int ilo, int jlo, int ihi, int jhi,
                                  Array4<Real> const& phi, Real rhs0, Real rhs1,
                                  Array4<Real> const& acf, Real facx, Real facy)
            {
                Real a = acf(i,j,0);
                gs1(i, j, 0, ilo, jlo, ihi, jhi, phi, rhs0, a, facx, facy);
                gs1(i, j, 1, ilo, jlo, ihi, jhi, phi, rhs1, a, facx, facy);
            },
            [=] AMREX_GPU_DEVICE (int i, int j, Real& res0, Real& res1,
                                  int ilo, int jlo, int ihi, int jhi,
                                  Array4<Real> const& phi, Real rhs0, Real rhs1,
                                  Array4<Real> const& acf, Real facx, Real facy)
            {
                Real a = acf(i,j,0);
                res0 = residual1(i, j, 0, ilo, jlo, ihi, jhi, phi, rhs0, a, facx, facy);
                res1 = residual1(i, j, 1, ilo, jlo, ihi, jhi, phi, rhs1, a, facx, facy);
            });
    } else {
        bottomsolve_gpu<nsweeps>(dx0, dy0, acf, res, cor, rescor, nlevs, corner_offset,
            [=] AMREX_GPU_DEVICE (int i, int j, int ilo, int jlo, int ihi, int jhi,
                                  Array4<Real> const& phi, Real rhs0, Real rhs1,
                                  Array4<Real> const& acf, Real facx, Real facy)
            {
                Real ar = acf(i,j,0,0);
                Real ai = acf(i,j,0,1);
                gs2(i, j, ilo, jlo, ihi, jhi, phi, rhs0, rhs1, ar, ai, facx, facy);
            },
            [=] AMREX_GPU_DEVICE (int i, int j, Real& res0, Real& res1,
                                  int ilo, int jlo, int ihi, int jhi,
                                  Array4<Real> const& phi, Real rhs_r, Real rhs_i,
                                  Array4<Real> const& acf, Real facx, Real facy)
            {
                Real ar = acf(i,j,0,0);
                Real ai = acf(i,j,0,1);
                res0 = residual2r(i, j, ilo, jlo, ihi, jhi, phi, rhs_r, ar, ai, facx, facy);
                res1 = residual2i(i, j, ilo, jlo, ihi, jhi, phi, rhs_i, ar, ai, facx, facy);
            });
    }
#else
    const int ilev = m_single_block_level_begin;
    m_cor[ilev].setVal(Real(0.));
    for (int is = 0; is < nsweeps; ++is) {
        gsrb(is, m_domain[ilev], m_cor[ilev].array(),
             m_res[ilev].const_array(), m_acf[ilev].const_array(), dx0, dy0,
             m_system_type);
    }
#endif
}

#if defined(AMREX_USE_GPU)
namespace {
    template <typename F>
    void avgdown_acf (Array4<Real> const* acf, int ncomp, int nlevels, F&& f)
    {
#if defined(AMREX_USE_DPCPP)
        amrex::launch(1, 1024, Gpu::gpuStream(),
        [=] (sycl::nd_item<1> const& item) noexcept
#else
        amrex::launch_global<1024><<<1, 1024, 0, Gpu::gpuStream()>>>(
        [=] AMREX_GPU_DEVICE () noexcept
#endif
        {
            for (int ilev = 1; ilev < nlevels; ++ilev) {
                const int lenx = acf[ilev].end.x - acf[ilev].begin.x;
                const int leny = acf[ilev].end.y - acf[ilev].begin.y;
                const int ncells = lenx*leny;
#if defined(AMREX_USE_DPCPP)
                for (int icell = item.get_local_range(0)*item.get_group_linear_id()
                         + item.get_local_linear_id(),
                         stride = item.get_local_range(0)*item.get_group_range(0);
#else
                for (int icell = blockDim.x*blockIdx.x+threadIdx.x, stride = blockDim.x*gridDim.x;
#endif
                     icell < ncells; icell += stride) {
                    int j = icell /   lenx;
                    int i = icell - j*lenx;
                    j += acf[ilev].begin.y;
                    i += acf[ilev].begin.x;
                    for (int n = 0; n < ncomp; ++n) {
                        f(i,j,n,acf[ilev],acf[ilev-1]);
                    }
                }
                HPMG_SYNCTHREADS;
            }
        });
    }
}
#endif

void
MultiGrid::average_down_acoef ()
{
    const int ncomp = (m_system_type == 1) ? 1 : 2;
#if defined(AMREX_USE_CUDA)
    const int igraph = m_system_type-1;
    bool& graph_created = m_cuda_graph_acf_created[igraph];
    cudaGraph_t& graph = m_cuda_graph_acf[igraph];
    cudaGraphExec_t& graph_exe = m_cuda_graph_exe_acf[igraph];
    if (!graph_created) {
    cudaStreamBeginCapture(Gpu::gpuStream(), cudaStreamCaptureModeGlobal);
#endif

    for (int ilev = 1; ilev <= m_single_block_level_begin; ++ilev) {
        auto const& crse = m_acf[ilev].array();
        auto const& fine = m_acf[ilev-1].const_array();
        if (m_domain[ilev].cellCentered()) {
            hpmg::ParallelFor(m_domain[ilev], ncomp,
            [=] AMREX_GPU_DEVICE (int i, int j, int, int n) noexcept
            {
                restrict_cc(i,j,n,crse,fine);
            });
        } else {
            hpmg::ParallelFor(valid_domain_box(m_domain[ilev]), ncomp,
            [=] AMREX_GPU_DEVICE (int i, int j, int, int n) noexcept
            {
                restrict_nd(i,j,n,crse,fine);
            });
        }
    }

#if defined(AMREX_USE_GPU)
    if (m_num_single_block_levels > 1) {
        if (m_domain[0].cellCentered()) {
            avgdown_acf(m_acf_a, ncomp, m_num_single_block_levels,
                        [] AMREX_GPU_DEVICE (int i, int j, int n, Array4<Real> const& crse,
                                             Array4<Real> const& fine) noexcept
                        {
                            restrict_cc(i,j,n,crse,fine);
                        });
        } else {
            avgdown_acf(m_acf_a, ncomp, m_num_single_block_levels,
                        [] AMREX_GPU_DEVICE (int i, int j, int n, Array4<Real> const& crse,
                                             Array4<Real> const& fine) noexcept
                        {
                            if (i == crse.begin.x ||
                                j == crse.begin.y ||
                                i == crse.end.x-1 ||
                                j == crse.end.y-1) {
                                crse(i,j,0,n) = Real(0.);
                            } else {
                                restrict_nd(i,j,n,crse,fine);
                            }
                        });
        }
    }
#endif

#if defined(AMREX_USE_CUDA)
    cudaStreamEndCapture(Gpu::gpuStream(), &graph);
    cudaGraphInstantiate(&graph_exe, graph, NULL, NULL, 0);
    graph_created = true;
    }
    cudaGraphLaunch(graph_exe, Gpu::gpuStream());
#endif
}

MultiGrid::~MultiGrid ()
{
#if defined(AMREX_USE_CUDA)
    for (int igraph = 0; igraph < m_num_system_types; ++igraph) {
        if (m_cuda_graph_acf_created[igraph]) {
            cudaGraphDestroy(m_cuda_graph_acf[igraph]);
            cudaGraphExecDestroy(m_cuda_graph_exe_acf[igraph]);
        }
        if (m_cuda_graph_vcycle_created[igraph]) {
            cudaGraphDestroy(m_cuda_graph_vcycle[igraph]);
            cudaGraphExecDestroy(m_cuda_graph_exe_vcycle[igraph]);
        }
    }
#endif
}

}
