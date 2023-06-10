/* Copyright 2020-2022
 *
 * This file is part of HiPACE++.
 *
 * Authors: AlexanderSinn, MaxThevenet, Severin Diederichs
 * License: BSD-3-Clause-LBNL
 */
#include "diagnostics/OpenPMDWriter.H"
#include "diagnostics/Diagnostic.H"
#include "fields/Fields.H"
#include "utils/HipaceProfilerWrapper.H"
#include "utils/Constants.H"
#include "utils/IOUtil.H"
#include "Hipace.H"

#ifdef HIPACE_USE_OPENPMD

OpenPMDWriter::OpenPMDWriter ()
{
    AMREX_ALWAYS_ASSERT_WITH_MESSAGE(m_real_names.size() == BeamIdx::real_nattribs,
        "List of real names in openPMD Writer class do not match BeamIdx::real_nattribs");
    amrex::ParmParse pp("hipace");
    queryWithParser(pp, "openpmd_backend", m_openpmd_backend);
    // pick first available backend if default is chosen
    if( m_openpmd_backend == "default" ) {
#if openPMD_HAVE_HDF5==1
        m_openpmd_backend = "h5";
#elif openPMD_HAVE_ADIOS2==1
        m_openpmd_backend = "bp";
#else
        m_openpmd_backend = "json";
#endif
    }

    // set default output path according to backend
    if (m_openpmd_backend == "h5") {
        m_file_prefix = "diags/hdf5";
    } else if (m_openpmd_backend == "bp") {
        m_file_prefix = "diags/adios2";
    } else if (m_openpmd_backend == "json") {
        m_file_prefix = "diags/json";
    }
    // overwrite output path by choice of the user
    queryWithParser(pp, "file_prefix", m_file_prefix);

    // temporary workaround until openPMD-viewer gets fixed
    amrex::ParmParse ppd("diagnostic");
    queryWithParser(ppd, "openpmd_viewer_u_workaround", m_openpmd_viewer_workaround);
}

void
OpenPMDWriter::InitDiagnostics ()
{
    HIPACE_PROFILE("OpenPMDWriter::InitDiagnostics()");

    std::string filename = m_file_prefix + "/openpmd_%06T." + m_openpmd_backend;

    m_outputSeries = std::make_unique< openPMD::Series >(
        filename, openPMD::Access::CREATE);
    m_last_beam_output_dumped = -1;

    // TODO: meta-data: author, mesh path, extensions, software
}

void
OpenPMDWriter::WriteDiagnostics (
    const amrex::Vector<FieldDiagnosticData>& field_diag, MultiBeam& a_multi_beam,
    const amrex::Real physical_time, const int output_step,
    const amrex::Vector< std::string > beamnames, const int it,
    amrex::Vector<amrex::Geometry> const& geom3D,
    const OpenPMDWriterCallType call_type)
{
    openPMD::Iteration iteration = m_outputSeries->iterations[output_step];
    iteration.setTime(physical_time);

    if (call_type == OpenPMDWriterCallType::beams ) {
        const int lev = 0;
        WriteBeamParticleData(a_multi_beam, iteration, output_step, it,
                              geom3D[lev], beamnames);
        m_last_beam_output_dumped = output_step;
        m_outputSeries->flush();
    } else if (call_type == OpenPMDWriterCallType::fields) {
        for (const auto& fd : field_diag) {
            if (fd.m_has_field) {
                WriteFieldData(fd.m_F, fd.m_geom_io, fd.m_slice_dir, fd.m_comps_output,
                           iteration);
            }
        }
        m_outputSeries->flush();
    }
}

void
OpenPMDWriter::WriteFieldData (
    amrex::FArrayBox const& fab, amrex::Geometry const& geom,
    const int slice_dir, const amrex::Vector< std::string > varnames,
    openPMD::Iteration iteration)
{
    // todo: periodicity/boundary, field solver, particle pusher, etc.
    auto meshes = iteration.meshes;

    // loop over field components
    for ( int icomp = 0; icomp < varnames.size(); ++icomp )
    {
        //                      "B"                "x" (todo)
        //                      "Bx"               ""  (just for now)
        openPMD::Mesh field = meshes[varnames[icomp]];
        openPMD::MeshRecordComponent field_comp = field[openPMD::MeshRecordComponent::SCALAR];

        // meta-data
        field.setDataOrder(openPMD::Mesh::DataOrder::C);
        //   node staggering
        auto relative_cell_pos = utils::getRelativeCellPosition(fab);      // AMReX Fortran index order
        std::reverse(relative_cell_pos.begin(), relative_cell_pos.end()); // now in C order

        amrex::Box const data_box = fab.box();

        //   labels, spacing and offsets
        std::vector< std::string > axisLabels {"z", "y", "x"};
        auto dCells = utils::getReversedVec(geom.CellSize()); // dx, dy, dz
        amrex::Vector<double> finalproblo = {AMREX_D_DECL(
                     static_cast<double>(geom.ProbLo()[2]),
                     static_cast<double>(geom.ProbLo()[1]),
                     static_cast<double>(geom.ProbLo()[0])
                      )};
        auto offWindow = finalproblo;
        if (slice_dir >= 0) {
            // User requested slice IO
            // remove the slicing direction in position, label, resolution, offset
            relative_cell_pos.erase(relative_cell_pos.begin() + 2-slice_dir);
            axisLabels.erase(axisLabels.begin() + 2-slice_dir);
            dCells.erase(dCells.begin() + 2-slice_dir);
            offWindow.erase(offWindow.begin() + 2-slice_dir);
        }
        field_comp.setPosition(relative_cell_pos);
        field.setAxisLabels(axisLabels);
        field.setGridSpacing(dCells);
        field.setGridGlobalOffset(offWindow);

        // data type and global size of the simulation
        openPMD::Datatype datatype = openPMD::determineDatatype< amrex::Real >();
        amrex::Vector<std::uint64_t> probsize_reformat = {AMREX_D_DECL(
                     static_cast<std::uint64_t>(geom.Domain().size()[2]),
                     static_cast<std::uint64_t>(geom.Domain().size()[1]),
                     static_cast<std::uint64_t>(geom.Domain().size()[0]))};
        openPMD::Extent global_size = probsize_reformat;
        // If slicing requested, remove number of points for the slicing direction
        if (slice_dir >= 0) global_size.erase(global_size.begin() + 2-slice_dir);

        openPMD::Dataset dataset(datatype, global_size);
        field_comp.resetDataset(dataset);

        // Determine the offset and size of this data chunk in the global output
        amrex::IntVect const box_offset =
            {0, 0, data_box.smallEnd(2) - geom.Domain().smallEnd(2)};
        openPMD::Offset chunk_offset = utils::getReversedVec(box_offset);
        openPMD::Extent chunk_size = utils::getReversedVec(data_box.size());
        if (slice_dir >= 0) { // remove Ny components
            chunk_offset.erase(chunk_offset.begin() + 2-slice_dir);
            chunk_size.erase(chunk_size.begin() + 2-slice_dir);
        }

        field_comp.storeChunkRaw(fab.dataPtr(icomp), chunk_offset, chunk_size);
    }
}

void
OpenPMDWriter::WriteBeamParticleData (MultiBeam& beams, openPMD::Iteration iteration,
                                      const int output_step, const int it,
                                      const amrex::Geometry& geom,
                                      const amrex::Vector< std::string > beamnames)
{
    HIPACE_PROFILE("WriteBeamParticleData()");

    const int nbeams = beams.get_nbeams();
    m_offset.resize(nbeams);
    m_tmp_offset.resize(nbeams);
    for (int ibeam = 0; ibeam < nbeams; ibeam++) {

        std::string name = beams.get_name(ibeam);
        if(std::find(beamnames.begin(), beamnames.end(), name) ==  beamnames.end() ) continue;

        openPMD::ParticleSpecies beam_species = iteration.particles[name];

        auto& beam = beams.getBeam(ibeam);

        const unsigned long long np = beams.get_total_num_particles(ibeam);
        if (m_last_beam_output_dumped != output_step) {
            SetupPos(beam_species, beam, np, geom);
            SetupRealProperties(beam_species, m_real_names, np);
        }

        // if first box of loop over boxes, reset offset
        if ( it == amrex::ParallelDescriptor::NProcs() -1 ) {
            m_offset[ibeam] = 0;
            m_tmp_offset[ibeam] = 0;
        } else {
            m_offset[ibeam] += m_tmp_offset[ibeam];
        }
        const uint64_t box_offset = beam.m_box_sorter.boxOffsetsPtr()[it];

        auto const numParticleOnTile = beam.m_box_sorter.boxCountsPtr()[it];
        uint64_t const numParticleOnTile64 = static_cast<uint64_t>( numParticleOnTile );

        if (numParticleOnTile == 0) {
            m_tmp_offset[ibeam] = 0;
            continue;
        }

        {
            // save particle ID
            std::shared_ptr< uint64_t > ids( new uint64_t[numParticleOnTile],
                                             [](uint64_t const *p){ delete[] p; } );

            for (uint64_t i=0; i<numParticleOnTile; i++) {
                ids.get()[i] = beam.id(i);
            }
            auto const scalar = openPMD::RecordComponent::SCALAR;
            beam_species["id"][scalar].storeChunk(ids, {m_offset[ibeam]}, {numParticleOnTile64});
        }
        //  save "extra" particle properties in SoA (momenta and weight)
        SaveRealProperty(beam, beam_species, m_offset[ibeam], m_real_names, box_offset,
                         numParticleOnTile);

         m_tmp_offset[ibeam] = numParticleOnTile64;
    }
}

void
OpenPMDWriter::SetupPos (openPMD::ParticleSpecies& currSpecies, BeamParticleContainer& beam,
                         const unsigned long long& np, const amrex::Geometry& geom)
{
    const PhysConst phys_const_SI = make_constants_SI();
    auto const realType = openPMD::Dataset(openPMD::determineDatatype<amrex::ParticleReal>(), {np});
    auto const idType = openPMD::Dataset(openPMD::determineDatatype< uint64_t >(), {np});

    std::vector< std::string > const positionComponents{"x", "y", "z"};
    for( auto const& comp : positionComponents ) {
        currSpecies["positionOffset"][comp].resetDataset( realType );
        currSpecies["positionOffset"][comp].makeConstant( 0. );
        currSpecies["position"][comp].resetDataset( realType );
    }

    auto const scalar = openPMD::RecordComponent::SCALAR;
    currSpecies["id"][scalar].resetDataset( idType );
    currSpecies["charge"][scalar].resetDataset( realType );
    currSpecies["charge"][scalar].makeConstant( beam.m_charge );
    currSpecies["mass"][scalar].resetDataset( realType );
    currSpecies["mass"][scalar].makeConstant( beam.m_mass );

    // meta data
    currSpecies["position"].setUnitDimension( utils::getUnitDimension("position") );
    currSpecies["positionOffset"].setUnitDimension( utils::getUnitDimension("positionOffset") );
    currSpecies["charge"].setUnitDimension( utils::getUnitDimension("charge") );
    currSpecies["mass"].setUnitDimension( utils::getUnitDimension("mass") );

    // calculate the multiplier to convert from Hipace to SI units
    double hipace_to_SI_pos = 1.;
    double hipace_to_SI_weight = 1.;
    double hipace_to_SI_momentum = beam.m_mass;
    double hipace_to_unitSI_momentum = beam.m_mass;
    double hipace_to_SI_charge = 1.;
    double hipace_to_SI_mass = 1.;

    if(Hipace::m_normalized_units) {
        const auto dx = geom.CellSizeArray();
        const double n_0 = 1.;
        currSpecies.setAttribute("HiPACE++_Plasma_Density", n_0);
        const double omega_p = (double)phys_const_SI.q_e * sqrt( (double)n_0 /
                                      ( (double)phys_const_SI.ep0 * (double)phys_const_SI.m_e ) );
        const double kp_inv = (double)phys_const_SI.c / omega_p;
        hipace_to_SI_pos = kp_inv;
        hipace_to_SI_weight = n_0 * dx[0] * dx[1] * dx[2] * kp_inv * kp_inv * kp_inv;
        hipace_to_SI_momentum = beam.m_mass * phys_const_SI.m_e * phys_const_SI.c;
        hipace_to_SI_charge = phys_const_SI.q_e;
        hipace_to_SI_mass = phys_const_SI.m_e;
    }

    // temporary workaround until openPMD-viewer does not autonormalize momentum
    if(m_openpmd_viewer_workaround) {
        if(Hipace::m_normalized_units) {
            hipace_to_unitSI_momentum = beam.m_mass * phys_const_SI.c;
        }
    }

    // write SI conversion
    currSpecies.setAttribute("HiPACE++_use_reference_unitSI", true);
    const std::string attr = "HiPACE++_reference_unitSI";
    for( auto const& comp : positionComponents ) {
        currSpecies["position"][comp].setAttribute( attr, hipace_to_SI_pos );
        //posOffset allways 0
        currSpecies["positionOffset"][comp].setAttribute( attr, hipace_to_SI_pos );
        currSpecies["momentum"][comp].setAttribute( attr, hipace_to_SI_momentum );
        currSpecies["momentum"][comp].setUnitSI( hipace_to_unitSI_momentum );
    }
    currSpecies["weighting"][scalar].setAttribute( attr, hipace_to_SI_weight );
    currSpecies["charge"][scalar].setAttribute( attr, hipace_to_SI_charge );
    currSpecies["mass"][scalar].setAttribute( attr, hipace_to_SI_mass );
}

void
OpenPMDWriter::SetupRealProperties (openPMD::ParticleSpecies& currSpecies,
                                    const amrex::Vector<std::string>& real_comp_names,
                                    const unsigned long long np)
{
    auto particlesLineup = openPMD::Dataset(openPMD::determineDatatype<amrex::ParticleReal>(),{np});

    /* we have 4 SoA real attributes: weight, ux, uy, uz */
    int const NumSoARealAttributes = real_comp_names.size();
    std::set< std::string > addedRecords; // add meta-data per record only once

    for (int i = 0; i < NumSoARealAttributes; ++i)
    {
        // handle scalar and non-scalar records by name
        std::string record_name, component_name;
        std::tie(record_name, component_name) = utils::name2openPMD(real_comp_names[i]);

        auto particleVarComp = currSpecies[record_name][component_name];
        particleVarComp.resetDataset(particlesLineup);

        auto currRecord = currSpecies[record_name];

        // meta data for ED-PIC extension
        bool newRecord = false;
        std::tie(std::ignore, newRecord) = addedRecords.insert(record_name);
        if( newRecord ) {
            currRecord.setUnitDimension( utils::getUnitDimension(record_name) );
            currRecord.setAttribute( "macroWeighted", 0u );
        if( record_name == "momentum" )
            currRecord.setAttribute( "weightingPower", 1.0 );
        else
            currRecord.setAttribute( "weightingPower", 0.0 );
        } // end if newRecord
    } // end for NumSoARealAttributes
}

void
OpenPMDWriter::SaveRealProperty (BeamParticleContainer& pc,
                                 openPMD::ParticleSpecies& currSpecies,
                                 unsigned long long const offset,
                                 amrex::Vector<std::string> const& real_comp_names,
                                 unsigned long long const box_offset,
                                 const unsigned long long numParticleOnTile)
{
    /* we have 4 SoA real attributes: weight, ux, uy, uz */
    int const NumSoARealAttributes = real_comp_names.size();

    uint64_t const numParticleOnTile64 = static_cast<uint64_t>( numParticleOnTile );
    auto const& soa = pc.GetStructOfArrays();
    {
        for (int idx=0; idx<NumSoARealAttributes; idx++) {

            // handle scalar and non-scalar records by name
            std::string record_name, component_name;
            std::tie(record_name, component_name) = utils::name2openPMD(real_comp_names[idx]);
            auto& currRecord = currSpecies[record_name];
            auto& currRecordComp = currRecord[component_name];

            currRecordComp.storeChunkRaw(soa.GetRealData(idx).data()+box_offset,
                                         {offset}, {numParticleOnTile64});
        } // end for NumSoARealAttributes
    }
}

void OpenPMDWriter::reset ()
{
    m_outputSeries.reset();
}

#endif // HIPACE_USE_OPENPMD
