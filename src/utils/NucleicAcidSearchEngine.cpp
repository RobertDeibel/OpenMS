// --------------------------------------------------------------------------
//                   OpenMS -- Open-Source Mass Spectrometry
// --------------------------------------------------------------------------
// Copyright The OpenMS Team -- Eberhard Karls University Tuebingen,
// ETH Zurich, and Freie Universitaet Berlin 2002-2017.
//
// This software is released under a three-clause BSD license:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of any author or any participating institution
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
// For a full list of authors, refer to the file AUTHORS.
// --------------------------------------------------------------------------
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL ANY OF THE AUTHORS OR THE CONTRIBUTING
// INSTITUTIONS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// --------------------------------------------------------------------------
// $Maintainer: Timo Sachsenberg $
// $Authors: Timo Sachsenberg, Samuel Wein $
// --------------------------------------------------------------------------

#include <OpenMS/KERNEL/StandardTypes.h>
#include <OpenMS/CONCEPT/Constants.h>

#include <OpenMS/APPLICATIONS/TOPPBase.h>
#include <OpenMS/DATASTRUCTURES/Param.h>

#include <OpenMS/KERNEL/MSExperiment.h>
#include <OpenMS/KERNEL/MSSpectrum.h>
#include <OpenMS/METADATA/SpectrumSettings.h>

// file types
#include <OpenMS/FORMAT/IdXMLFile.h>
#include <OpenMS/FORMAT/MzMLFile.h>
#include <OpenMS/FORMAT/FASTAFile.h>

// digestion enzymes
#include <OpenMS/CHEMISTRY/EnzymaticDigestion.h>

/*
#include <OpenMS/CHEMISTRY/DigestionEnzymeRNA.h>
#include <OpenMS/CHEMISTRY/RNaseDB.h>
*/

// ribonucleotides
#include <OpenMS/CHEMISTRY/RibonucleotideDB.h>
#include <OpenMS/CHEMISTRY/ElementDB.h>
#include <OpenMS/CHEMISTRY/ModifiedNASequenceGenerator.h>
#include <OpenMS/CHEMISTRY/NASequence.h>

// preprocessing and filtering
#include <OpenMS/FILTERING/TRANSFORMERS/ThresholdMower.h>
#include <OpenMS/FILTERING/TRANSFORMERS/NLargest.h>
#include <OpenMS/FILTERING/TRANSFORMERS/WindowMower.h>
#include <OpenMS/FILTERING/TRANSFORMERS/Normalizer.h>

// spectra comparison
#include <OpenMS/CHEMISTRY/TheoreticalSpectrumGenerator.h>
#include <OpenMS/COMPARISON/SPECTRA/SpectrumAlignment.h>

#include <OpenMS/KERNEL/Peak1D.h>
#include <OpenMS/DATASTRUCTURES/ListUtils.h>
#include <OpenMS/DATASTRUCTURES/String.h>

#include <boost/regex.hpp>

#include <QtCore/QProcess>

#include <iostream>
#include <algorithm>
#include <vector>
#include <map>


#ifdef _OPENMP
#include <omp.h>
#include <OpenMS/ANALYSIS/ID/MetaboliteSpectralMatching.h>

#define NUMBER_OF_THREADS (omp_get_num_threads())
#else
#define NUMBER_OF_THREADS (1)
#endif

//#define DEBUG_NASEARCH

using namespace OpenMS;
using namespace std;

// TODO: adapt to new datastructure
struct SequenceLessComparator
{
  bool operator()(const PeptideHit& a, const PeptideHit& b)
  {
    if (a.getSequence().toString() < b.getSequence().toString()) { return true; }
    return false;
  }
};

class NucleicAcidSearchEngine :
  public TOPPBase
{
  using ConstRibonucleotidePtr = const Ribonucleotide *;
public:
  NucleicAcidSearchEngine() :
    TOPPBase("NucleicAcidSearchEngine", "Annotate nucleic acid identifications to MS/MS spectra.", false)
  {
  }

protected:
  void registerOptionsAndFlags_()
  {
    registerInputFile_("in", "<file>", "", "input file ");
    setValidFormats_("in", ListUtils::create<String>("mzML"));

    registerInputFile_("database", "<file>", "", "input file ");
    setValidFormats_("database", ListUtils::create<String>("fasta"));

    registerOutputFile_("out", "<file>", "", "output file ");
    setValidFormats_("out", ListUtils::create<String>("idXML"));

    registerTOPPSubsection_("precursor", "Precursor (Parent Ion) Options");
    registerDoubleOption_("precursor:mass_tolerance", "<tolerance>", 10.0, "Precursor mass tolerance (+/- around precursor m/z)", false);

    StringList precursor_mass_tolerance_unit_valid_strings;
    precursor_mass_tolerance_unit_valid_strings.push_back("ppm");
    precursor_mass_tolerance_unit_valid_strings.push_back("Da");

    registerStringOption_("precursor:mass_tolerance_unit", "<unit>", "ppm", "Unit of precursor mass tolerance.", false, false);
    setValidStrings_("precursor:mass_tolerance_unit", precursor_mass_tolerance_unit_valid_strings);

    registerIntOption_("precursor:min_charge", "<num>", 2, "Minimum precursor charge to be considered.", false, false);
    registerIntOption_("precursor:max_charge", "<num>", 5, "Maximum precursor charge to be considered.", false, false);

    // consider one before annotated monoisotopic peak and the annotated one
    IntList isotopes = {0, 1};
    registerIntList_("precursor:isotopes", "<num>", isotopes, "Corrects for mono-isotopic peak misassignments. (E.g.: 1 = prec. may be misassigned to first isotopic peak)", false, false);

    registerTOPPSubsection_("fragment", "Fragments (Product Ion) Options");
    registerDoubleOption_("fragment:mass_tolerance", "<tolerance>", 10.0, "Fragment mass tolerance (+/- around fragment m/z)", false);

    StringList fragment_mass_tolerance_unit_valid_strings;
    fragment_mass_tolerance_unit_valid_strings.push_back("ppm");
    fragment_mass_tolerance_unit_valid_strings.push_back("Da");

    registerStringOption_("fragment:mass_tolerance_unit", "<unit>", "ppm", "Unit of fragment m", false, false);
    setValidStrings_("fragment:mass_tolerance_unit", fragment_mass_tolerance_unit_valid_strings);

    registerTOPPSubsection_("modifications", "Modifications Options");

    // add modified ribos from databaser
    vector<String> all_mods;
    for (auto r : *RibonucleotideDB::getInstance())
    {
      if (r->isModified()) { all_mods.push_back(r->getCode());}
    }

    registerStringList_("modifications:fixed", "<mods>", ListUtils::create<String>(""), "Fixed modifications'", false);
    setValidStrings_("modifications:fixed", all_mods);
    registerStringList_("modifications:variable", "<mods>", ListUtils::create<String>(""), "Variable modifications", false);
    setValidStrings_("modifications:variable", all_mods);
    registerIntOption_("modifications:variable_max_per_oligo", "<num>", 2, "Maximum number of residues carrying a variable modification per candidate oligo", false, false);

    registerTOPPSubsection_("oligo", "Oligo Options");
    registerIntOption_("oligo:min_size", "<num>", 6, "Minimum size a oligo must have after digestion to be considered in the search.", false, true);
    registerIntOption_("oligo:missed_cleavages", "<num>", 1, "Number of missed cleavages.", false, false);

    StringList all_enzymes;
    RNaseDB::getInstance()->getAllNames(all_enzymes);
    registerStringOption_("oligo:enzyme", "<cleavage site>", "Trypsin", "The enzyme used for oligo digestion.", false);
    setValidStrings_("oligo:enzyme", all_enzymes);


    registerTOPPSubsection_("report", "Reporting Options");
    registerIntOption_("report:top_hits", "<num>", 1, "Maximum number of top scoring hits per spectrum that are reported.", false, true);
  }

  // slimmer structure to store basic hit information
  struct AnnotatedHit
  {
    StringView sequence; // pointer into sequence database
    SignedSize mod_index; // enumeration index of the modification
    double score; // the score
    static bool hasBetterScore(const AnnotatedHit& a, const AnnotatedHit& b) { return a.score > b.score; }
  };

  // query modified residues from database
  vector<ConstRibonucleotidePtr> getModifications_(StringList modNames)
  {
    vector<ConstRibonucleotidePtr> modifications;
    for (auto const & m : modNames)
    {
      String modification(m);
      ConstRibonucleotidePtr rm = RibonucleotideDB::getInstance()->getRibonucleotide(modification);
      modifications.push_back(rm);
    }
    return modifications;
  }

  // check for minimum size
  class HasInvalidLength
  {
    Size min_size_;
  public:
    explicit HasInvalidLength(Size min_size)
      : min_size_(min_size)
    {
    }
    bool operator()(const NASequence& s) { return s.size() < min_size_; }
  };

  // spectrum must not contain 0 intensity peaks and must be sorted by m/z
  void deisotopeAndSingleChargeMSSpectrum(
    MSSpectrum& in,
    Int min_charge,
    Int max_charge,
    double fragment_tolerance,
    bool fragment_unit_ppm,
    bool keep_only_deisotoped = false,
    Size min_isopeaks = 3,
    Size max_isopeaks = 10,
    bool make_single_charged = true)
  {
    if (in.empty()) { return; }

    MSSpectrum old_spectrum = in;

    // determine charge seeds and extend them
    vector<Size> mono_isotopic_peak(old_spectrum.size(), 0);
    vector<Int> features(old_spectrum.size(), -1);
    Int feature_number = 0;

    for (Size current_peak = 0; current_peak != old_spectrum.size(); ++current_peak)
    {
      double current_mz = old_spectrum[current_peak].getPosition()[0];

      for (Int q = max_charge; q >= min_charge; --q) // important: test charge hypothesis from high to low
      {
        // try to extend isotopes from mono-isotopic peak
        // if extension larger then min_isopeaks possible:
        //   - save charge q in mono_isotopic_peak[]
        //   - annotate all isotopic peaks with feature number
        if (features[current_peak] == -1) // only process peaks which have no assigned feature number
        {
          bool has_min_isopeaks = true;
          vector<Size> extensions;
          for (Size i = 0; i < max_isopeaks; ++i)
          {
            double expected_mz = current_mz + i * Constants::C13C12_MASSDIFF_U / q;
            Size p = old_spectrum.findNearest(expected_mz);
            double tolerance_dalton = fragment_unit_ppm ? fragment_tolerance * old_spectrum[p].getPosition()[0] * 1e-6 : fragment_tolerance;
            if (fabs(old_spectrum[p].getPosition()[0] - expected_mz) > tolerance_dalton) // test for missing peak
            {
              if (i < min_isopeaks)
              {
                has_min_isopeaks = false;
              }
              break;
            }
            else
            {
/*
              // TODO: include proper averagine model filtering. for now start at the second peak to test hypothesis
              Size n_extensions = extensions.size();
              if (n_extensions != 0)
              {
                if (old_spectrum[p].getIntensity() > old_spectrum[extensions[n_extensions - 1]].getIntensity())
                {
                  if (i < min_isopeaks)
                  {
                    has_min_isopeaks = false;
                  }
                  break;
                }
              }

*/
              // averagine check passed
              extensions.push_back(p);
            }
          }

          if (has_min_isopeaks)
          {
            //cout << "min peaks at " << current_mz << " " << " extensions: " << extensions.size() << endl;
            mono_isotopic_peak[current_peak] = q;
            for (Size i = 0; i != extensions.size(); ++i)
            {
              features[extensions[i]] = feature_number;
            }
            feature_number++;
          }
        }
      }
    }

    in.clear(false);
    for (Size i = 0; i != old_spectrum.size(); ++i)
    {
      Int z = mono_isotopic_peak[i];
      if (keep_only_deisotoped)
      {
        if (z == 0)
        {
          continue;
        }

        // if already single charged or no decharging selected keep peak as it is
        if (!make_single_charged)
        {
          in.push_back(old_spectrum[i]);
        }
        else
        {
          Peak1D p = old_spectrum[i];
          p.setMZ(p.getMZ() * z - (z - 1) * Constants::PROTON_MASS_U);
          in.push_back(p);
        }
      }
      else
      {
        // keep all unassigned peaks
        if (features[i] < 0)
        {
          in.push_back(old_spectrum[i]);
          continue;
        }

        // convert mono-isotopic peak with charge assigned by deisotoping
        if (z != 0)
        {
          if (!make_single_charged)
          {
            in.push_back(old_spectrum[i]);
          }
          else
          {
            Peak1D p = old_spectrum[i];
            p.setMZ(p.getMZ() * z - (z - 1) * Constants::PROTON_MASS_U);
            in.push_back(p);
          }
        }
      }
    }

    in.sortByPosition();
  }

  void preprocessSpectra_(PeakMap& exp, double fragment_mass_tolerance, bool fragment_mass_tolerance_unit_ppm, bool single_charge_spectra)
  {
    // filter MS2 map
    // remove 0 intensities
    ThresholdMower threshold_mower_filter;
    threshold_mower_filter.filterPeakMap(exp);

    Normalizer normalizer;
    normalizer.filterPeakMap(exp);

    // sort by rt
    exp.sortSpectra(false);

    // filter settings
    WindowMower window_mower_filter;
    Param filter_param = window_mower_filter.getParameters();
    filter_param.setValue("windowsize", 100.0, "The size of the sliding window along the m/z axis.");

    // Note: we expect a higher number for NA than e.g., for peptides
    filter_param.setValue("peakcount", 50, "The number of peaks that should be kept.");
    filter_param.setValue("movetype", "jump", "Whether sliding window (one peak steps) or jumping window (window size steps) should be used.");
    window_mower_filter.setParameters(filter_param);

    // Note: we expect a higher number for NA than e.g., for peptides
    NLargest nlargest_filter = NLargest(1000);

#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (SignedSize exp_index = 0; exp_index < (SignedSize)exp.size(); ++exp_index)
    {
      // sort by mz
      exp[exp_index].sortByPosition();

      // deisotope
      deisotopeAndSingleChargeMSSpectrum_(exp[exp_index], 1, 20, fragment_mass_tolerance, fragment_mass_tolerance_unit_ppm, false, 3, 20, true);

      // remove noise
      window_mower_filter.filterPeakSpectrum(exp[exp_index]);
      nlargest_filter.filterPeakSpectrum(exp[exp_index]);

      // sort (nlargest changes order)
      exp[exp_index].sortByPosition();
    }
  }

  struct FragmentAnnotationDetail_
  {
    int charge;
    double mz;
    double intensity;

    bool operator<(const FragmentAnnotationDetail_& other) const
    {
      return std::tie(charge, mz, intensity) <
             std::tie(other.charge, other.mz, other.intensity);
    }

    bool operator==(const FragmentAnnotationDetail_& other) const
    {
      double mz_diff = fabs(mz - other.mz);
      double intensity_diff = fabs(intensity - other.intensity);
      // mz and intensity difference comparison actually not needed but kept for completeness
      return (mz_diff < 1e-6 && intensity_diff < 1e-6);
    }
  };

  void postProcessHits_(const PeakMap& exp,
                        vector<vector<AnnotatedHit> >& annotated_hits,
                        vector<ProteinIdentification>& protein_ids,
                        PeptideIdentifications& oligo_ids,  // TODO: adapt to new datastructures
                        Size top_hits,
                        const vector<ConstRibonucleotidePtr>& fixed_modifications,
                        const vector<ConstRibonucleotidePtr>& variable_modifications,
                        Size max_variable_mods_per_oligo)
  {
    // remove all but top n scoring
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (SignedSize scan_index = 0; scan_index < (SignedSize)annotated_hits.size(); ++scan_index)
    {
      // sort and keeps n best elements according to score
      Size topn = top_hits > annotated_hits[scan_index].size() ? annotated_hits[scan_index].size() : top_hits;
      std::partial_sort(annotated_hits[scan_index].begin(), annotated_hits[scan_index].begin() + topn, annotated_hits[scan_index].end(), AnnotatedHit::hasBetterScore);
      annotated_hits[scan_index].resize(topn);
    }

#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (SignedSize scan_index = 0; scan_index < (SignedSize)annotated_hits.size(); ++scan_index)
    {
      if (!annotated_hits[scan_index].empty())
      {
        // create empty PeptideIdentification object and fill meta data
        PeptideIdentification pi;
        pi.setMetaValue("scan_index", static_cast<unsigned int>(scan_index));
        pi.setScoreType("hyperscore");
        pi.setHigherScoreBetter(true);
        pi.setRT(exp[scan_index].getRT());
        pi.setMZ(exp[scan_index].getPrecursors()[0].getMZ());
        Size charge = exp[scan_index].getPrecursors()[0].getCharge();

        // create full oligo hit structure from annotated hits
        vector<PeptideHit> phs;
        for (vector<AnnotatedHit>::const_iterator a_it = annotated_hits[scan_index].begin(); a_it != annotated_hits[scan_index].end(); ++a_it)
        {
          PeptideHit ph;
          ph.setCharge(charge);

          // get unmodified string
          NASequence ns = NASequence::fromString(a_it->sequence.getString(), Ribonucleotide::RNA);

          // reapply modifications (because for memory reasons we only stored the index and recreation is fast)
          vector<NASequence> all_modified_oligos;
          ModifiedNASequenceGenerator::applyFixedModifications(fixed_modifications.begin(), fixed_modifications.end(), ns);
          ModifiedNASequenceGenerator::applyVariableModifications(variable_modifications.begin(),
                                                               variable_modifications.end(),
                                                               ns,
                                                               max_variable_mods_per_oligo,
                                                               all_modified_oligos);

          // reannotate much more memory heavy NASequence object
          NASequence fixed_and_variable_modified_oligo = all_modified_oligos[a_it->mod_index];
          ph.setScore(a_it->score);

          ph.setSequence(fixed_and_variable_modified_oligo); // TODO: adapt to new datastructures
          phs.push_back(ph);
        }

        pi.setHits(phs);
        pi.assignRanks();

#ifdef _OPENMP
#pragma omp critical (oligo_ids_access)
#endif
        {
          oligo_ids.push_back(pi);
        }
      }
    }

    // protein identifications (leave as is...)
    protein_ids = vector<ProteinIdentification>(1);
    protein_ids[0].setDateTime(DateTime::now());
    protein_ids[0].setSearchEngine("NucleicAcidSearchEngine");
    protein_ids[0].setSearchEngineVersion(VersionInfo::getVersion());
    ProteinIdentification::SearchParameters search_parameters;
    search_parameters.db = getStringOption_("database");
    search_parameters.charges = String(getIntOption_("precursor:min_charge")) + ":" + String(getIntOption_("precursor:max_charge"));
    search_parameters.missed_cleavages = getIntOption_("oligo:missed_cleavages");
    search_parameters.fragment_mass_tolerance = getDoubleOption_("fragment:mass_tolerance");
    search_parameters.precursor_mass_tolerance = getDoubleOption_("precursor:mass_tolerance");
    protein_ids[0].setSearchParameters(search_parameters);
  }

  ExitCodes main_(int, const char**)
  {
    ProgressLogger progresslogger;
    progresslogger.setLogType(log_type_);
    String in_mzml = getStringOption_("in");
    String in_db = getStringOption_("database");
    String out_idxml = getStringOption_("out");

    Int min_precursor_charge = getIntOption_("precursor:min_charge");
    Int max_precursor_charge = getIntOption_("precursor:max_charge");
    double precursor_mass_tolerance = getDoubleOption_("precursor:mass_tolerance");
    bool precursor_mass_tolerance_unit_ppm = (getStringOption_("precursor:mass_tolerance_unit") == "ppm");
    IntList precursor_isotopes = getIntList_("precursor:isotopes");

    double fragment_mass_tolerance = getDoubleOption_("fragment:mass_tolerance");
    bool fragment_mass_tolerance_unit_ppm = (getStringOption_("fragment:mass_tolerance_unit") == "ppm");

    StringList fixedModNames = getStringList_("modifications:fixed");
    set<String> fixed_unique(fixedModNames.begin(), fixedModNames.end());

    Size oligo_min_size = getIntOption_("oligo:min_size");

    if (fixed_unique.size() != fixedModNames.size())
    {
      LOG_WARN << "duplicate fixed modification provided." << endl;
      return ILLEGAL_PARAMETERS;
    }

    StringList varModNames = getStringList_("modifications:variable");
    set<String> var_unique(varModNames.begin(), varModNames.end());
    if (var_unique.size() != varModNames.size())
    {
      LOG_WARN << "duplicate variable modification provided." << endl;
      return ILLEGAL_PARAMETERS;
    }

    vector<ConstRibonucleotidePtr> fixed_modifications = getModifications_(fixedModNames);
    vector<ConstRibonucleotidePtr> variable_modifications = getModifications_(varModNames);
    Size max_variable_mods_per_oligo = getIntOption_("modifications:variable_max_per_oligo");

    Int report_top_hits = getIntOption_("report:top_hits");

    // load MS2 map
    PeakMap spectra;
    MzMLFile f;
    f.setLogType(log_type_);

    PeakFileOptions options;
    options.clearMSLevels();
    options.addMSLevel(2);
    f.getOptions() = options;
    f.load(in_mzml, spectra);
    spectra.sortSpectra(true);

    progresslogger.startProgress(0, 1, "Filtering spectra...");
    preprocessSpectra_(spectra, fragment_mass_tolerance, fragment_mass_tolerance_unit_ppm, true);
    progresslogger.endProgress();

    // build multimap of precursor mass to scan index
    // build multimap of precursor mass to scan index
    multimap<double, Size> multimap_mass_2_scan_index;
    for (PeakMap::ConstIterator s_it = spectra.begin(); s_it != spectra.end(); ++s_it)
    {
      int scan_index = s_it - spectra.begin();
      vector<Precursor> precursor = s_it->getPrecursors();

      // there should only one precursor and MS2 should contain at least a few peaks to be considered (e.g. at least for every ribo in the chain)
      if (precursor.size() == 1 && s_it->size() >= oligo_min_size)
      {
        int precursor_charge = precursor[0].getCharge();

        if (precursor_charge < min_precursor_charge || precursor_charge > max_precursor_charge)
        {
          continue;
        }

        double precursor_mz = precursor[0].getMZ();

        // calculate precursor mass (optionally corrected for misassignment) and map it to MS scan index
        for (int isotope_number : precursor_isotopes)
        {
          double precursor_mass = (double) precursor_charge * precursor_mz - (double) precursor_charge * Constants::PROTON_MASS_U;

          // correct for monoisotopic misassignments of the precursor annotation
          if (isotope_number != 0) { precursor_mass -= isotope_number * Constants::C13C12_MASSDIFF_U; }

          multimap_mass_2_scan_index.insert(make_pair(precursor_mass, scan_index));
        }
      }
    }

    // create spectrum generator
    TheoreticalSpectrumGenerator spectrum_generator;
    Param param(spectrum_generator.getParameters());
    param.setValue("add_first_prefix_ion", "true");
    param.setValue("add_metainfo", "true");
    spectrum_generator.setParameters(param);

    vector<vector<AnnotatedHit> > annotated_hits(spectra.size(), vector<AnnotatedHit>());

    progresslogger.startProgress(0, 1, "Load database from FASTA file...");
    FASTAFile fastaFile;
    vector<FASTAFile::FASTAEntry> fasta_db;
    fastaFile.load(in_db, fasta_db);
    progresslogger.endProgress();

    const Size missed_cleavages = getIntOption_("oligo:missed_cleavages");
    EnzymaticDigestion digestor;
    digestor.setEnzyme(getStringOption_("oligo:enzyme"));
    digestor.setMissedCleavages(missed_cleavages);

    progresslogger.startProgress(0, (Size)(fasta_db.end() - fasta_db.begin()), "Scoring oligo models against spectra...");

    // lookup for processed oligos. must be defined outside of omp section and synchronized
    set<StringView> processed_oligos;

    // set minimum size of oligo after digestion
    Size min_oligo_length = getIntOption_("oligo:min_size");

    Size count_undigested = 0;
    Size count_oligos = 0;

#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (int fasta_index = 0; fasta_index < (int)fasta_db.size(); ++fasta_index)
    {
#ifdef _OPENMP
#pragma omp atomic
#endif
      ++count_undigested;

      IF_MASTERTHREAD
      {
        progresslogger.setProgress((int)fasta_index * NUMBER_OF_THREADS);
      }

      vector<StringView> current_digest;
      digestor.digestUnmodifiedString(fasta_db[fasta_index].sequence, current_digest, min_oligo_length);

      for (vector<StringView>::iterator cit = current_digest.begin(); cit != current_digest.end(); ++cit)
      {
        bool already_processed = false;
#ifdef _OPENMP
#pragma omp critical (processed_oligos_access)
#endif
        {
          // oligo (and all modified variants) already processed so skip it
          if (processed_oligos.find(*cit) != processed_oligos.end())
          {
            already_processed = true;
          }
        }

        if (already_processed) { continue; }

#ifdef _OPENMP
#pragma omp critical (processed_oligos_access)
#endif
        {
          processed_oligos.insert(*cit);
        }

#ifdef _OPENMP
#pragma omp atomic
#endif
        ++count_oligos;
        vector<NASequence> all_modified_oligos;

        // no critial section is needed despite ResidueDB not beeing thread sage.
        // It is only written to on introduction of novel modified residues. These residues have been already added above (single thread context).
        {
          NASequence ns = NASequence::fromString(cit->getString(), Ribonucleotide::RNA);
          ModifiedNASequenceGenerator::applyFixedModifications(fixed_modifications.begin(),
                                                               fixed_modifications.end(),
                                                               ns);
          ModifiedNASequenceGenerator::applyVariableModifications(variable_modifications.begin(),
                                                                  variable_modifications.end(),
                                                                  ns,
                                                                  max_variable_mods_per_oligo,
                                                                  all_modified_oligos,
                                                                  true);
        }

        for (SignedSize mod_idx = 0; mod_idx < (SignedSize)all_modified_oligos.size(); ++mod_idx)
        {
          const NASequence& candidate = all_modified_oligos[mod_idx];
          double candidate_mass = candidate.getMonoWeight();

          // determine MS2 precursors that match to the current mass
          multimap<double, Size>::const_iterator low_it;
          multimap<double, Size>::const_iterator up_it;

          if (precursor_mass_tolerance_unit_ppm) // ppm
          {
            low_it = multimap_mass_2_scan_index.lower_bound(candidate_mass - 0.5 * candidate_mass * precursor_mass_tolerance * 1e-6);
            up_it = multimap_mass_2_scan_index.upper_bound(candidate_mass + 0.5 * candidate_mass * precursor_mass_tolerance * 1e-6);
          }
          else // Dalton
          {
            low_it = multimap_mass_2_scan_index.lower_bound(candidate_mass - 0.5 * precursor_mass_tolerance);
            up_it = multimap_mass_2_scan_index.upper_bound(candidate_mass + 0.5 * precursor_mass_tolerance);
          }

          if (low_it == up_it)
          {
            continue;     // no matching precursor in data
          }

          //create theoretical spectrum
          PeakSpectrum theo_spectrum;

          //add peaks for b and y ions with charge 1
          spectrum_generator.getSpectrum(theo_spectrum, candidate, 1, 1);

          //sort by mz
          theo_spectrum.sortByPosition();

          for (; low_it != up_it; ++low_it)
          {
            const Size& scan_index = low_it->second;
            const PeakSpectrum& exp_spectrum = spectra[scan_index];

            double score = MetaboliteSpectralMatching::computeHyperScore(
              fragment_mass_tolerance,
              fragment_mass_tolerance_unit_ppm,
              exp_spectrum,
              theo_spectrum);

            // no hit
            if (score < 1e-16)
            {
              continue;
            }

            // add oligo hit
            AnnotatedHit ah;
            ah.sequence = *cit;
            ah.mod_index = mod_idx;
            ah.score = score;

#ifdef DEBUG_NASEARCH
            cout << "best score in pre-score: " << score << endl;
#endif

#ifdef _OPENMP
#pragma omp critical (annotated_hits_access)
#endif
            {
              annotated_hits[scan_index].push_back(ah);
            }
          }
        }
      }
    }
    progresslogger.endProgress();

    LOG_INFO << "Undigested Oligos: " << count_undigested << std::endl;
    LOG_INFO << "Oligos: " << count_oligos << std::endl;
    LOG_INFO << "Processed oligos: " << processed_oligos.size() << std::endl;

    PeptideIdentifications oligo_ids;  // TODO: adapt to new datatstructures
    vector<ProteinIdentification> protein_ids;

    progresslogger.startProgress(0, 1, "annotation...");
    postProcessHits_(
      spectra,
      annotated_hits,
      protein_ids,
      oligo_ids,
      report_top_hits,
      fixed_modifications,
      variable_modifications,
      max_variable_mods_per_oligo);
    progresslogger.endProgress();

    // TODO: reindex oligo to undigested sequence

    // store results
    IdXMLFile().store(out_idxml, protein_ids, oligo_ids);

    return EXECUTION_OK;
  }

};

int main(int argc, const char** argv)
{
  NucleicAcidSearchEngine tool;
  return tool.main(argc, argv);
}
