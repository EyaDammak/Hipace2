#include "diagnostics/OpenPMDWriter.H"
#include "fields/Fields.H"
#include "utils/HipaceProfilerWrapper.H"
#include "utils/Constants.H"
#include "utils/IOUtil.H"

#ifdef HIPACE_USE_OPENPMD

OpenPMDWriter::OpenPMDWriter ()
{
    AMREX_ALWAYS_ASSERT_WITH_MESSAGE(m_real_names.size() == BeamIdx::nattribs,
        "List of real names in openPMD Writer class do not match BeamIdx::nattribs");
    amrex::ParmParse pp("hipace");
    pp.query("file_prefix", m_file_prefix);
}

void
OpenPMDWriter::InitDiagnostics ()
{
    HIPACE_PROFILE("OpenPMDWriter::InitDiagnostics()");

    std::string filename = m_file_prefix + "/openpmd_%06T.h5"; // bp or h5

    m_outputSeries = std::make_unique< openPMD::Series >(
        filename, openPMD::Access::CREATE);

    // TODO: meta-data: author, mesh path, extensions, software
}

void
OpenPMDWriter::WriteDiagnostics (
    amrex::Vector<amrex::FArrayBox> const& a_mf, MultiBeam& a_multi_beam,
    amrex::Vector<amrex::Geometry> const& geom,
    const amrex::Real physical_time, const int output_step, const int lev,
    const int slice_dir, const amrex::Vector< std::string > varnames, const int it,
    const amrex::Vector<BoxSorter>& a_box_sorter_vec)
{
    openPMD::Iteration iteration = m_outputSeries->iterations[output_step];
    iteration.setTime(physical_time);

    WriteFieldData(a_mf[lev], geom[lev], slice_dir, varnames, iteration, output_step);

    a_multi_beam.ConvertUnits(ConvertDirection::HIPACE_to_SI);
    WriteBeamParticleData(a_multi_beam, iteration, output_step, it, a_box_sorter_vec);

    m_outputSeries->flush();

    // back conversion after the flush, to not change the data to be written to file
    a_multi_beam.ConvertUnits(ConvertDirection::SI_to_HIPACE);

    m_last_output_dumped = output_step;
}

void
OpenPMDWriter::WriteFieldData (
    amrex::FArrayBox const& fab, amrex::Geometry const& geom,
    const int slice_dir, const amrex::Vector< std::string > varnames,
    openPMD::Iteration iteration, const int output_step)
{
    // todo: periodicity/boundary, field solver, particle pusher, etc.
    auto meshes = iteration.meshes;

    // loop over field components
    for (std::string fieldname : varnames)
    {
        int icomp = Comps[WhichSlice::This][fieldname];
        //                      "B"                "x" (todo)
        //                      "Bx"               ""  (just for now)
        openPMD::Mesh field = meshes[fieldname];
        openPMD::MeshRecordComponent field_comp = field[openPMD::MeshRecordComponent::SCALAR];

        // meta-data
        field.setDataOrder(openPMD::Mesh::DataOrder::C);
        //   node staggering
        auto relative_cell_pos = utils::getRelativeCellPosition(fab);      // AMReX Fortran index order
        std::reverse(relative_cell_pos.begin(), relative_cell_pos.end()); // now in C order
        //   labels, spacing and offsets
        std::vector< std::string > axisLabels {"z", "y", "x"};
        auto dCells = utils::getReversedVec(geom.CellSize()); // dx, dy, dz
        auto offWindow = utils::getReversedVec(geom.ProbLo()); // start of moving window
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
        openPMD::Extent global_size = utils::getReversedVec(geom.Domain().size());
        // If slicing requested, remove number of points for the slicing direction
        if (slice_dir >= 0) global_size.erase(global_size.begin() + 2-slice_dir);

        if (m_last_output_dumped != output_step) {
            openPMD::Dataset dataset(datatype, global_size);
            field_comp.resetDataset(dataset);
        }

        // Store the provided box as a chunk with openpmd
        amrex::Box const data_box = fab.box();
        std::shared_ptr< amrex::Real const > data;

        data = openPMD::shareRaw( fab.dataPtr( icomp ) ); // non-owning view until flush()


        // Determine the offset and size of this data chunk in the global output
        amrex::IntVect const box_offset = data_box.smallEnd();
        openPMD::Offset chunk_offset = utils::getReversedVec(box_offset);
        openPMD::Extent chunk_size = utils::getReversedVec(data_box.size());
        if (slice_dir >= 0) { // remove Ny components
            chunk_offset.erase(chunk_offset.begin() + 2-slice_dir);
            chunk_size.erase(chunk_size.begin() + 2-slice_dir);
        }

        field_comp.storeChunk(data, chunk_offset, chunk_size);
    }
}

void
OpenPMDWriter::WriteBeamParticleData (MultiBeam& beams, openPMD::Iteration iteration,
                                      const int output_step, const int it,
                                      const amrex::Vector<BoxSorter>& a_box_sorter_vec)
{
    HIPACE_PROFILE("WriteBeamParticleData()");

    const int nbeams = beams.get_nbeams();
    m_offset.resize(nbeams);
    m_tmp_offset.resize(nbeams);
    for (int ibeam = 0; ibeam < nbeams; ibeam++) {

        std::string name = beams.get_name(ibeam);
        openPMD::ParticleSpecies beam_species = iteration.particles[name];

        const unsigned long long np = beams.get_total_num_particles(ibeam);
        if (m_last_output_dumped != output_step) {
            SetupPos(beam_species, np);
            SetupRealProperties(beam_species, m_real_names, np);
        }

        // if first box of loop over boxes, reset offset
        if ( it == amrex::ParallelDescriptor::NProcs() -1 ) {
            m_offset[ibeam] = 0;
            m_tmp_offset[ibeam] = 0;
        } else {
            m_offset[ibeam] += m_tmp_offset[ibeam];
        }
        const uint64_t box_offset = a_box_sorter_vec[ibeam].boxOffsetsPtr()[it];
        // Loop over particle boxes NOTE: Only 1 particle box allowed at the moment
        auto& beam = beams.getBeam(ibeam);

        auto const numParticleOnTile = a_box_sorter_vec[ibeam].boxCountsPtr()[it];
        uint64_t const numParticleOnTile64 = static_cast<uint64_t>( numParticleOnTile );

        if (numParticleOnTile == 0) {
            m_tmp_offset[ibeam] = 0;
            continue;
        }

        // get position and particle ID from aos
        // note: this implementation iterates the AoS 4x...
        // if we flush late as we do now, we can also copy out the data in one go
        const auto& aos = beam.GetArrayOfStructs();  // size =  numParticlesOnTile
        const auto& pos_structs = aos.begin() + box_offset;
        {
            // Save positions
            std::vector< std::string > const positionComponents{"x", "y", "z"};

            for (auto currDim = 0; currDim < AMREX_SPACEDIM; currDim++)
            {
                std::shared_ptr< amrex::ParticleReal > curr(
                    new amrex::ParticleReal[numParticleOnTile],
                    [](amrex::ParticleReal const *p){ delete[] p; } );

                for (auto i=0; i<numParticleOnTile; i++) {
                    curr.get()[i] = pos_structs[i].pos(currDim);
                }
                std::string const positionComponent = positionComponents[currDim];
                beam_species["position"][positionComponent].storeChunk(curr, {m_offset[ibeam]},
                                                                       {numParticleOnTile64});
            }

            // save particle ID after converting it to a globally unique ID
            std::shared_ptr< uint64_t > ids( new uint64_t[numParticleOnTile],
                                             [](uint64_t const *p){ delete[] p; } );

            for (auto i=0; i<numParticleOnTile; i++) {
                ids.get()[i] = utils::localIDtoGlobal( aos[i].id(), aos[i].cpu() );
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
OpenPMDWriter::SetupPos (openPMD::ParticleSpecies& currSpecies,
                         const unsigned long long& np)
{
    PhysConst const phys_const = get_phys_const();
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
    currSpecies["charge"][scalar].makeConstant( phys_const.q_e );
    currSpecies["mass"][scalar].resetDataset( realType );
    currSpecies["mass"][scalar].makeConstant( phys_const.m_e );

    // meta data
    currSpecies["position"].setUnitDimension( utils::getUnitDimension("position") );
    currSpecies["positionOffset"].setUnitDimension( utils::getUnitDimension("positionOffset") );
    currSpecies["charge"].setUnitDimension( utils::getUnitDimension("charge") );
    currSpecies["mass"].setUnitDimension( utils::getUnitDimension("mass") );
}

void
OpenPMDWriter::SetupRealProperties (openPMD::ParticleSpecies& currSpecies,
                                    const amrex::Vector<std::string>& real_comp_names,
                                    const unsigned long long np)
{
    auto particlesLineup = openPMD::Dataset(openPMD::determineDatatype<amrex::ParticleReal>(), {np});

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

            currRecordComp.storeChunk(openPMD::shareRaw(soa.GetRealData(idx).data()+box_offset),
                {offset}, {numParticleOnTile64});
        } // end for NumSoARealAttributes
    }
}

#endif // HIPACE_USE_OPENPMD
