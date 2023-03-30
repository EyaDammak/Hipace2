/* Copyright 2021-2022
 *
 * This file is part of HiPACE++.
 *
 * Authors: AlexanderSinn, MaxThevenet, Severin Diederichs
 * License: BSD-3-Clause-LBNL
 */
#include "Diagnostic.H"
#include "Hipace.H"
#include <AMReX_ParmParse.H>

Diagnostic::Diagnostic (int nlev)
    : m_F(nlev),
      m_diag_coarsen(nlev),
      m_geom_io(nlev),
      m_has_field(nlev)
{
    amrex::ParmParse ppd("diagnostic");
    std::string str_type;
    getWithParser(ppd, "diag_type", str_type);
    if        (str_type == "xyz"){
        m_diag_type = DiagType::xyz;
        m_slice_dir = -1;
    } else if (str_type == "xz") {
        m_diag_type = DiagType::xz;
        m_slice_dir = 1;
    } else if (str_type == "yz") {
        m_diag_type = DiagType::yz;
        m_slice_dir = 0;
    } else {
        amrex::Abort("Unknown diagnostics type: must be xyz, xz or yz.");
    }

    queryWithParser(ppd, "include_ghost_cells", m_include_ghost_cells);

    m_use_custom_size_lo = queryWithParser(ppd, "patch_lo", m_diag_lo);
    m_use_custom_size_hi = queryWithParser(ppd, "patch_hi", m_diag_hi);

    for(int ilev = 0; ilev<nlev; ++ilev) {
        amrex::Array<int,3> diag_coarsen_arr{1,1,1};
        // set all levels the same for now
        queryWithParser(ppd, "coarsening", diag_coarsen_arr);
        if(m_slice_dir == 0 || m_slice_dir == 1) {
            diag_coarsen_arr[m_slice_dir] = 1;
        }
        m_diag_coarsen[ilev] = amrex::IntVect(diag_coarsen_arr);
        AMREX_ALWAYS_ASSERT_WITH_MESSAGE( m_diag_coarsen[ilev].min() >= 1,
            "Coarsening ratio must be >= 1");
    }
}

void
Diagnostic::Initialize (const int lev, bool do_laser) {
    if (lev!=0) return;

    m_do_laser = do_laser;
    amrex::ParmParse ppd("diagnostic");
    queryWithParser(ppd, "field_data", m_comps_output);
    amrex::Vector<std::string> all_field_comps{};
    all_field_comps.reserve(Comps[WhichSlice::This].size());
    for (const auto& [comp, idx] : Comps[WhichSlice::This]) {
        all_field_comps.push_back(comp);
    }
    if(m_comps_output.empty()) {
        m_comps_output = all_field_comps;
    }
    else {
        for(std::string comp_name : m_comps_output) {
            if(comp_name == "all" || comp_name == "All") {
                m_comps_output = all_field_comps;
                break;
            }
            if(comp_name == "none" || comp_name == "None") {
                m_comps_output.clear();
                break;
            }
            if(Comps[WhichSlice::This].count(comp_name) == 0) {
                std::stringstream error_str{};
                error_str << "Unknown field diagnostics component: " << comp_name << "\nmust be "
                    << "'all', 'none' or a subset of:";
                for (auto& comp : all_field_comps) {
                    error_str << " " << comp;
                }
                amrex::Abort(error_str.str());
            }
        }
    }
    m_nfields = m_comps_output.size();
    m_comps_output_idx = amrex::Gpu::DeviceVector<int>(m_nfields);
    for(int i = 0; i < m_nfields; ++i) {
        m_comps_output_idx[i] = Comps[WhichSlice::This][m_comps_output[i]];
    }

    amrex::ParmParse ppb("beams");
    // read in all beam names
    amrex::Vector<std::string> all_beam_names;
    queryWithParser(ppb, "names", all_beam_names);
    // read in which beam should be written to file
    queryWithParser(ppd, "beam_data", m_output_beam_names);

    if(m_output_beam_names.empty()) {
        m_output_beam_names = all_beam_names;
    } else {
        for(std::string beam_name : m_output_beam_names) {
            if(beam_name == "all" || beam_name == "All") {
                m_output_beam_names = all_beam_names;
                break;
            }
            if(beam_name == "none" || beam_name == "None") {
                m_output_beam_names.clear();
                break;
            }
            if(std::find(all_beam_names.begin(), all_beam_names.end(), beam_name) ==  all_beam_names.end() ) {
                amrex::Abort("Unknown beam name: " + beam_name + "\nmust be " +
                "a subset of beams.names or 'none'");
            }
        }
    }

    m_initialized = true;
}

void
Diagnostic::ResizeFDiagFAB (amrex::Box local_box, amrex::Box domain, const int lev,
                            amrex::Geometry const& geom)
{
    AMREX_ALWAYS_ASSERT(m_initialized);

    if (m_include_ghost_cells) {
        local_box.grow(Fields::m_slices_nguards);
        domain.grow(Fields::m_slices_nguards);
    }

    {
        // shrink box to user specified bounds m_diag_lo and m_diag_hi (in real space)
        const amrex::Real poff_x = GetPosOffset(0, geom, geom.Domain());
        const amrex::Real poff_y = GetPosOffset(1, geom, geom.Domain());
        const amrex::Real poff_z = GetPosOffset(2, geom, geom.Domain());
        amrex::Box cut_domain = domain;
        if (m_use_custom_size_lo) {
            cut_domain.setSmall({
                static_cast<int>(std::round((m_diag_lo[0] - poff_x)/geom.CellSize(0))),
                static_cast<int>(std::round((m_diag_lo[1] - poff_y)/geom.CellSize(1))),
                static_cast<int>(std::round((m_diag_lo[2] - poff_z)/geom.CellSize(2)))
            });
        }
        if (m_use_custom_size_hi) {
            cut_domain.setBig({
                static_cast<int>(std::round((m_diag_hi[0] - poff_x)/geom.CellSize(0))),
                static_cast<int>(std::round((m_diag_hi[1] - poff_y)/geom.CellSize(1))),
                static_cast<int>(std::round((m_diag_hi[2] - poff_z)/geom.CellSize(2)))
            });
        }
        // calculate intersection of boxes to prevent them getting larger
        domain &= cut_domain;
        local_box &= domain;
    }

    amrex::RealBox diag_domain = geom.ProbDomain();
    for(int dir=0; dir<=2; ++dir) {
        // make diag_domain correspond to box
        diag_domain.setLo(dir, geom.ProbLo(dir)
            + (domain.smallEnd(dir) - geom.Domain().smallEnd(dir)) * geom.CellSize(dir));
        diag_domain.setHi(dir, geom.ProbHi(dir)
            + (domain.bigEnd(dir) - geom.Domain().bigEnd(dir)) * geom.CellSize(dir));
    }
    // trim the 3D box to slice box for slice IO
    TrimIOBox(local_box, domain, diag_domain);

    local_box.coarsen(m_diag_coarsen[lev]);
    domain.coarsen(m_diag_coarsen[lev]);

    m_geom_io[lev] = amrex::Geometry(domain, &diag_domain, geom.Coord());

    m_has_field[lev] = local_box.ok();

    if(m_has_field[lev]) {
        m_F[lev].resize(local_box, getTotalNFields(), amrex::The_Pinned_Arena());
        m_F[lev].setVal<amrex::RunOn::Host>(0);
    }
}

void
Diagnostic::TrimIOBox (amrex::Box& box_3d, amrex::Box& domain_3d, amrex::RealBox& rbox_3d)
{
    if (m_slice_dir >= 0){
        const amrex::Real half_cell_size = rbox_3d.length(m_slice_dir) /
                                           ( 2. * domain_3d.length(m_slice_dir) );
        const amrex::Real mid = (rbox_3d.lo(m_slice_dir) + rbox_3d.hi(m_slice_dir)) / 2.;
        // Flatten the box down to 1 cell in the approprate direction.
        box_3d.setSmall(m_slice_dir, 0);
        box_3d.setBig  (m_slice_dir, 0);
        domain_3d.setSmall(m_slice_dir, 0);
        domain_3d.setBig  (m_slice_dir, 0);
        rbox_3d.setLo(m_slice_dir, mid - half_cell_size);
        rbox_3d.setHi(m_slice_dir, mid + half_cell_size);
    }
}
