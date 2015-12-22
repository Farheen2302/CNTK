//
// <copyright file="utterancesource.h" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
// utterancesource.h -- implementation of a two-level minibatch source ('minibatchutterancesource') that can provide lattices and frame blocks
//

#pragma once

#include <unordered_map>
#include "basetypes.h"                  // for attempt()
#include "htkfeatio.h"                  // for htkmlfreader
#include "latticearchive.h"             // for reading HTK phoneme lattices (MMI training)
#include "minibatchsourcehelpers.h"
#include "minibatchiterator.h"

namespace msra { namespace dbn {

// ---------------------------------------------------------------------------
// minibatchutterancesource -- feature source to provide randomized utterances
// This also implements a frame-wise mode, which is layered on top of the utterance-wise mode
// and thus benefits from its goodies such as corpus-wide high-level randomization and chunk paging.
// ---------------------------------------------------------------------------
class minibatchutterancesource : public minibatchsource
{
    void operator=(const minibatchutterancesource & other); // non-assignable
    size_t vdim;                    // feature dimension after augmenting neighhors
    size_t leftcontext;
    size_t rightcontext;
    unsigned int sampperiod;        // (for reference and to check against model)
    string featkind;
    size_t featdim;
    const bool framemode;           // true -> actually return frame-level randomized frames (not possible in lattice mode)
    std::vector<size_t> counts;     // [s] occurence count for all states (used for priors)
    int verbosity;
    // lattice reader
    const latticesource & lattices;

    // word-level transcripts (for MMI mode when adding best path to lattices)
    const map<wstring,msra::lattices::lattice::htkmlfwordsequence> & allwordtranscripts; // (used for getting word-level transcripts)

    // data store (incl. paging in/out of features and lattices)
    struct utterancedesc            // data descriptor for one utterance
    {
        msra::asr::htkfeatreader::parsedpath parsedpath;    // archive filename and frame range in that file
        size_t classidsbegin;       // index into allclassids[] array (first frame)

        utterancedesc (msra::asr::htkfeatreader::parsedpath && ppath, size_t classidsbegin) : parsedpath (ppath), classidsbegin (classidsbegin) {}

        const wstring & logicalpath() const { return parsedpath; /*type cast will return logical path*/ }
        size_t numframes() const { return parsedpath.numframes(); }
        const wstring key() const                           // key used for looking up lattice (not stored to save space)
        {
            static const wstring emptywstring;
            static const wregex deleteextensionre (L"\\.[^\\.\\\\/:]*$");
            return regex_replace (logicalpath(), deleteextensionre, emptywstring);  // delete extension (or not if none)
        }
    };
    struct utterancechunkdata       // data for a chunk of utterances
    {
        std::vector<utterancedesc> utteranceset;    // utterances in this set
        size_t numutterances() const { return utteranceset.size(); }

        std::vector<size_t> firstframes;    // [utteranceindex] first frame for given utterance
        mutable msra::dbn::matrix frames;   // stores all frames consecutively (mutable since this is a cache)
        size_t totalframes;         // total #frames for all utterances in this chunk
        mutable std::vector<shared_ptr<const latticesource::latticepair>> lattices;   // (may be empty if none)

        // construction
        utterancechunkdata() : totalframes (0) {}
        void push_back (utterancedesc &&/*destructive*/ utt)
        {
            if (isinram())
                throw std::logic_error ("utterancechunkdata: frames already paged into RAM--too late to add data");
            firstframes.push_back (totalframes);
            totalframes += utt.numframes();
            utteranceset.push_back (utt);
        }

        // accessors to an utterance's data
        size_t numframes (size_t i) const { return utteranceset[i].numframes(); }
        size_t getclassidsbegin (size_t i) const { return utteranceset[i].classidsbegin; }
        msra::dbn::matrixstripe getutteranceframes (size_t i) const // return the frame set for a given utterance
        {
            if (!isinram())
                throw std::logic_error ("getutteranceframes: called when data have not been paged in");
            const size_t ts = firstframes[i];
            const size_t n = numframes(i);
            return msra::dbn::matrixstripe (frames, ts, n);
        }
        shared_ptr<const latticesource::latticepair> getutterancelattice (size_t i) const // return the frame set for a given utterance
        {
            if (!isinram())
                throw std::logic_error ("getutteranceframes: called when data have not been paged in");
            return lattices[i];
        }

        // paging
        // test if data is in memory at the moment
        bool isinram() const { return !frames.empty(); }
        // page in data for this chunk
        // We pass in the feature info variables by ref which will be filled lazily upon first read
        void requiredata (string & featkind, size_t & featdim, unsigned int & sampperiod, const latticesource & latticesource) const
        {
            if (numutterances() == 0)
                throw std::logic_error ("requiredata: cannot page in virgin block");
            if (isinram())
                throw std::logic_error ("requiredata: called when data is already in memory");
            try             // this function supports retrying since we read from the unrealible network, i.e. do not return in a broken state
            {
                msra::asr::htkfeatreader reader;    // feature reader (we reinstantiate it for each block, i.e. we reopen the file actually)
                // if this is the first feature read ever, we explicitly open the first file to get the information such as feature dimension
                if (featdim == 0)
                {
                    reader.getinfo (utteranceset[0].parsedpath, featkind, featdim, sampperiod);
                    fprintf (stderr, "requiredata: determined feature kind as %zd-dimensional '%s' with frame shift %.1f ms\n", featdim, featkind.c_str(), sampperiod / 1e4);
                }
                // read all utterances; if they are in the same archive, htkfeatreader will be efficient in not closing the file
                frames.resize (featdim, totalframes);
                if (!latticesource.empty())
                    lattices.resize (utteranceset.size());
                foreach_index (i, utteranceset)
                {
                    //fprintf (stderr, ".");
                    // read features for this file
                    auto uttframes = getutteranceframes (i);    // matrix stripe for this utterance (currently unfilled)
                    reader.read (utteranceset[i].parsedpath, (const string &) featkind, sampperiod, uttframes);  // note: file info here used for checkuing only
                    // page in lattice data
                    if (!latticesource.empty())
                        latticesource.getlattices (utteranceset[i].key(), lattices[i], uttframes.cols());
                }
                //fprintf (stderr, "\n");
                fprintf (stderr, "requiredata: %zd utterances read\n", utteranceset.size());
            }
            catch (...)
            {
                releasedata();
                throw;
            }
        }
        // page out data for this chunk
        void releasedata() const
        {
            if (numutterances() == 0)
                throw std::logic_error ("releasedata: cannot page out virgin block");
            if (!isinram())
                throw std::logic_error ("releasedata: called when data is not memory");
            // release frames
            frames.resize (0, 0);
            // release lattice data
            lattices.clear();
        }
    };
    std::vector<utterancechunkdata> allchunks;          // set of utterances organized in chunks, referred to by an iterator (not an index)
    biggrowablevector<CLASSIDTYPE> classids;            // [classidsbegin+t] concatenation of all state sequences
    bool issupervised() const { return !classids.empty(); }
    size_t numutterances;           // total number of utterances
    size_t _totalframes;             // total frames (same as classids.size() if we have labels)
    double timegetbatch;            // [v-hansu] for time measurement
    // sequence in random order of actual use (randomized, where randomization is cached)
    const size_t randomizationrange;// parameter remembered; this is the full window (e.g. 48 hours), not the half window
    size_t currentsweep;            // randomization is currently cached for this sweep; if it changes, rebuild all below
    struct chunk                    // chunk as used in actual processing order (randomized sequence)
    {
        // the underlying chunk (as a non-indexed reference into the chunk set)
        std::vector<utterancechunkdata>::const_iterator uttchunkdata;
        const utterancechunkdata & getchunkdata() const { return *uttchunkdata; }
        size_t numutterances() const { return uttchunkdata->numutterances(); }
        size_t numframes() const { return uttchunkdata->totalframes; }

        // position in utterance-position space
        size_t utteranceposbegin;
        size_t utteranceposend() const { return utteranceposbegin + numutterances(); }

        // position on global time line
        size_t globalts;            // start frame on global timeline (after randomization)
        size_t globalte() const { return globalts + numframes(); }

        // randomization range limits
        size_t windowbegin;         // randomizedchunk index of earliest chunk that utterances in here can be randomized with
        size_t windowend;           // and end index [windowbegin, windowend)
        chunk (std::vector<utterancechunkdata>::const_iterator uttchunkdata, size_t utteranceposbegin, size_t globalts) : uttchunkdata (uttchunkdata), utteranceposbegin (utteranceposbegin), globalts (globalts) {}
    };
    std::vector<chunk> randomizedchunks;  // utterance chunks after being brought into random order (we randomize within a rolling window over them)
    size_t chunksinram;             // (for diagnostics messages)
    struct utteranceref             // describes the underlying random utterance associated with an utterance position
    {
        size_t chunkindex;          // lives in this chunk (index into randomizedchunks[])
        size_t utteranceindex;      // utterance index in that chunk
        size_t numframes;           // (cached since we cannot directly access the underlying data from here)
        size_t globalts;            // start frame in global space after randomization (for mapping frame index to utterance position)
        size_t globalte() const { return globalts + numframes; }            // end frame
        utteranceref (size_t chunkindex, size_t utteranceindex) : chunkindex (chunkindex), utteranceindex (utteranceindex), globalts (SIZE_MAX), numframes (0) {}
        void swap (utteranceref & other)   // used in randomization
        {
            ::swap (chunkindex, other.chunkindex);
            ::swap (utteranceindex, other.utteranceindex);
            assert (globalts == SIZE_MAX && other.globalts == SIZE_MAX && numframes == 0 && other.numframes == 0);    // can only swap before assigning these
        }
    };
    std::vector<utteranceref> randomizedutterancerefs;          // [pos] randomized utterance ids
    std::unordered_map<size_t,size_t> randomizedutteranceposmap;     // [globalts] -> pos lookup table
    struct positionchunkwindow       // chunk window required in memory when at a certain position, for controlling paging
    {
        std::vector<chunk>::iterator definingchunk;       // the chunk in randomizedchunks[] that defined the utterance position of this utterance
        size_t windowbegin() const { return definingchunk->windowbegin; }
        size_t windowend() const { return definingchunk->windowend; }
        bool isvalidforthisposition (const utteranceref & utt) const
        {
            return utt.chunkindex >= windowbegin() && utt.chunkindex < windowend(); // check if 'utt' lives in is in allowed range for this position
        }
        positionchunkwindow (std::vector<chunk>::iterator definingchunk) : definingchunk (definingchunk) {}
    };
    std::vector<positionchunkwindow> positionchunkwindows;      // [utterance position] -> [windowbegin, windowend) for controlling paging

    // frame-level randomization layered on top of utterance chunking (randomized, where randomization is cached)
    struct frameref
    {
#ifdef  _WIN64  // (sadly, the compiler makes this 8 bytes, not 6)
        unsigned short chunkindex;           // lives in this chunk (index into randomizedchunks[])
        unsigned short utteranceindex;       // utterance index in that chunk
        static const size_t maxutterancesperchunk = 65535;
        unsigned short frameindex;           // frame index within the utterance
        static const size_t maxframesperutterance = 65535;
#else   // For Win32, we care to keep it inside 32 bits. We have already encountered setups where that's not enough.
        unsigned int chunkindex : 13;           // lives in this chunk (index into randomizedchunks[])
        unsigned int utteranceindex : 8;        // utterance index in that chunk
        static const size_t maxutterancesperchunk = 255;
        unsigned int frameindex : 11;           // frame index within the utterance
        static const size_t maxframesperutterance = 2047;
#endif
        frameref(size_t ci, size_t ui, size_t fi) : chunkindex((unsigned short)ci), utteranceindex((unsigned short)ui), frameindex((unsigned short)fi)
        {
#ifndef  _WIN64
            static_assert (sizeof (frameref) == 4, "frameref: bit fields too large to fit into 32-bit integer");
#endif
            if (ci == chunkindex && ui == utteranceindex && fi == frameindex)
                return;
            throw std::logic_error ("frameref: bit fields too small");
        }
        frameref() : chunkindex (0), utteranceindex (0), frameindex (0) {}
    };
    biggrowablevector<frameref> randomizedframerefs;  // [globalt-sweepts] -> (chunk, utt, frame) lookup table for randomized frames  --this can be REALLY big!

    // TODO: this may go away if we store classids directly in the utterance data
    template<class VECTOR> class shiftedvector  // accessing a vector with a non-0 starting index
    {
        void operator= (const shiftedvector &);
        VECTOR & v;
        size_t first;
        size_t n;
        void check (size_t i) const { if (i >= n) throw std::logic_error ("shiftedvector: index out of bounds"); }
    public:
        shiftedvector (VECTOR & v, size_t first, size_t n) : v (v), first (first), n (n) { }
        // TODO: the following is not templated--do it if needed; also should return a const reference then
        size_t operator[] (size_t i) const { check (i); return v[first + i]; }
    };
    template<class UTTREF> shiftedvector<biggrowablevector<CLASSIDTYPE>> getclassids (const UTTREF & uttref)  // return sub-vector of classids[] for a given utterance
    {
        if (!issupervised())
            return shiftedvector<biggrowablevector<CLASSIDTYPE>> (classids, 0, 0);     // nothing to return
        const auto & chunk = randomizedchunks[uttref.chunkindex];
        const auto & chunkdata = chunk.getchunkdata();
        const size_t classidsbegin = chunkdata.getclassidsbegin (uttref.utteranceindex); // index of first state label in global concatenated classids[] array
        const size_t n = chunkdata.numframes (uttref.utteranceindex);
        if (classids[classidsbegin + n] != (CLASSIDTYPE) -1)
            throw std::logic_error ("getclassids: expected boundary marker not found, internal data structure screwed up");
        return shiftedvector<biggrowablevector<CLASSIDTYPE>> (classids, classidsbegin, n);     // nothing to return
    }
public:
    // constructor
    // Pass empty labels to denote unsupervised training (so getbatch() will not return uids).
    // This mode requires utterances with time stamps.
    minibatchutterancesource (const std::vector<wstring> & infiles, const map<wstring,std::vector<msra::asr::htkmlfentry>> & labels,
                              size_t vdim, size_t udim, size_t leftcontext, size_t rightcontext, size_t randomizationrange, const latticesource & lattices, const map<wstring,msra::lattices::lattice::htkmlfwordsequence> & allwordtranscripts, const bool framemode)
        : vdim (vdim), leftcontext(leftcontext), rightcontext(rightcontext), sampperiod (0), featdim (0), randomizationrange (randomizationrange), currentsweep (SIZE_MAX),
          lattices (lattices), allwordtranscripts (allwordtranscripts), framemode (framemode), chunksinram (0), timegetbatch (0), verbosity(2)    
        // [v-hansu] change framemode (lattices.empty()) into framemode (false) to run utterance mode without lattice
        // you also need to change another line, search : [v-hansu] comment out to run utterance mode without lattice
    {    
        // process infiles to know dimensions of things (but not loading features)
        std::vector<utterancedesc> utteranceset;// read all utterances to here first; at the end, distribute to chunks
        utteranceset.reserve (infiles.size());
        size_t nomlf = 0;                       // number of entries missing in MLF (diagnostics)
        size_t nolat = 0;                       // number of entries missing in lattice archive (diagnostics)
        size_t numclasses = 0;                  // number of output classes as found in the label file (diagnostics)
        _totalframes = 0;
        wstring key;
        foreach_index (i, infiles)
        {
            if (i % (infiles.size() / 100 + 1) == 0) { fprintf (stderr, "."); fflush (stderr); }
            // build utterance descriptor
            utterancedesc utterance (msra::asr::htkfeatreader::parsedpath (infiles[i]), labels.empty() ? 0 : classids.size() /*classidsbegin*/);
            const size_t uttframes = utterance.numframes(); // will throw if frame bounds not given --required to be given in this mode
            // we need at least 2 frames for boundary markers to work
            if (uttframes < 2)
                throw std::runtime_error ("minibatchutterancesource: utterances < 2 frames not supported");
            if (uttframes > frameref::maxframesperutterance)
            {
                fprintf (stderr, "minibatchutterancesource: skipping %d-th file (%zd frames) because it exceeds max. frames (%zd) for frameref bit field: %S", i, uttframes, frameref::maxframesperutterance, key.c_str());
                continue;
            }

            // check whether we have the ref transcript
            auto labelsiter = labels.end();
            if (!labels.empty())    // empty means unsupervised mode (don't load any)
            {
                key = utterance.key();
                // check if labels are available (if not, it normally means that no path was found in realignment)
                labelsiter = labels.find (key);
                const bool lacksmlf = (labelsiter == labels.end());
                if (lacksmlf)
                    if (nomlf++ < 5)
                        fprintf (stderr, " [no labels for  %S]", key.c_str());
                // check if lattice is available (when in lattice mode)
                // TODO: also check the #frames here; requires a design change of the TOC format & a rerun
                const bool lackslat = !lattices.empty() && !lattices.haslattice (key); // ('true' if we have no lattices)
                if (lackslat)
                    if (nolat++ < 5)
                        fprintf (stderr, " [no lattice for %S]", key.c_str());
                // skip if either one is missing
                if (lacksmlf || lackslat)
                    continue;   // skip this utterance at all
            }
            // push the label sequence into classids[], since we already looked it up
            // TODO: we can store labels more efficiently now since we don't do frame-wise random access anymore.
            if (labelsiter != labels.end())
            {
                const auto & labseq = labelsiter->second;
                // check if durations match; skip if not
                size_t labframes = labseq.empty() ? 0 : (labseq[labseq.size()-1].firstframe + labseq[labseq.size()-1].numframes);
                if (labframes != uttframes)
                {
                    fprintf (stderr, " [duration mismatch (%zd in label vs. %zd in feat file), skipping %S]", labframes, uttframes, key.c_str());
                    nomlf++;
                    continue;   // skip this utterance at all
                }
                // expand classid sequence into flat array
                foreach_index (i, labseq)
                {
                    const auto & e = labseq[i];
                    if ((i > 0 && labseq[i-1].firstframe + labseq[i-1].numframes != e.firstframe) || (i == 0 && e.firstframe != 0))
                        throw std::runtime_error (msra::strfun::strprintf ("minibatchutterancesource: labels not in consecutive order MLF in label set: %S", key.c_str()));
                    if (e.classid >= udim)
                        throw std::runtime_error (msra::strfun::strprintf ("minibatchutterancesource: class id %d exceeds model output dimension %d in file %S", e.classid, udim, key.c_str()));
                    if (e.classid != (CLASSIDTYPE) e.classid)
                        throw std::runtime_error ("CLASSIDTYPE has too few bits");
                    for (size_t t = e.firstframe; t < e.firstframe + e.numframes; t++)
                        classids.push_back ((CLASSIDTYPE) e.classid);
                    numclasses = max (numclasses, (size_t)(1u + e.classid));
                    counts.resize (numclasses, 0);
                    counts[e.classid] += e.numframes;
                }
                classids.push_back ((CLASSIDTYPE) -1);  // append a boundary marker marker for checking
            }
            // OK, utterance has all we need --remember it
            utteranceset.push_back (std::move (utterance));
            _totalframes += uttframes;
            if (!labels.empty() && classids.size() != _totalframes + utteranceset.size())
                throw std::logic_error (msra::strfun::strprintf ("minibatchutterancesource: label duration inconsistent with feature file in MLF label set: %S", key.c_str()));
            assert (labels.empty() || classids.size() == _totalframes + utteranceset.size());
        }
        fprintf (stderr, " %zd frames in %zd out of %zd utterances; %zd classes\n", _totalframes, utteranceset.size(),infiles.size(), numclasses);
        if (!labels.empty())
            foreach_index (i, utteranceset)
        {
                if (classids[utteranceset[i].classidsbegin + utteranceset[i].numframes()] != (CLASSIDTYPE) -1)
                    throw std::logic_error ("minibatchutterancesource: classids[] out of sync");
        }
        if (nomlf + nolat > 0)
        {
            fprintf (stderr, "minibatchutterancesource: out of %zd files, %zd files not found in label set and %zd have no lattice\n", infiles.size(), nomlf, nolat);
            if (nomlf + nolat > infiles.size() / 2)
                throw std::runtime_error ("minibatchutterancesource: too many files not found in label set--assuming broken configuration\n");
        }

        // distribute them over chunks
        // We simply count off frames until we reach the chunk size.
        // Note that we first randomize the chunks, i.e. when used, chunks are non-consecutive and thus cause the disk head to seek for each chunk.
        const size_t framespersec = 100;                    // we just assume this; our efficiency calculation is based on this
        const size_t chunkframes = 15 * 60 * framespersec;  // number of frames to target for each chunk
        // Loading an initial 24-hour range will involve 96 disk seeks, acceptable.
        // When paging chunk by chunk, chunk size ~14 MB.

        allchunks.resize (0);
        allchunks.reserve (_totalframes / chunkframes);

        foreach_index (i, utteranceset)
        {
            // if exceeding current entry--create a new one
            // I.e. our chunks are a little larger than wanted (on av. half the av. utterance length).
            if (allchunks.empty() || allchunks.back().totalframes > chunkframes || allchunks.back().numutterances() >= frameref::maxutterancesperchunk)
                allchunks.push_back (utterancechunkdata());
            // append utterance to last chunk
            utterancechunkdata & currentchunk = allchunks.back();
            currentchunk.push_back (std::move (utteranceset[i]));    // move it out from our temp array into the chunk
            // TODO: above push_back does not actually 'move' because the internal push_back does not accept that
        }
        numutterances = utteranceset.size();
        fprintf (stderr, "minibatchutterancesource: %zd utterances grouped into %zd chunks, av. chunk size: %.1f utterances, %.1f frames\n",
                 numutterances, allchunks.size(), numutterances / (double) allchunks.size(), _totalframes / (double) allchunks.size());
        // Now utterances are stored exclusively in allchunks[]. They are never referred to by a sequential utterance id at this point, only by chunk/within-chunk index.

        // preliminary mem allocation for frame references (if in frame mode)
        if (framemode)
            randomizedframerefs.resize (_totalframes);
    }

private:
    // shuffle a vector into random order by randomly swapping elements
    template<typename VECTOR> static void randomshuffle (VECTOR & v, size_t randomseed)
    {
        if (v.size() > RAND_MAX * (size_t) RAND_MAX)
            throw std::runtime_error ("randomshuffle: too large set: need to change to different random generator!");
        srand ((unsigned int) randomseed);
        foreach_index (i, v)
        {
            // pick a random location
            const size_t irand = msra::dbn::rand (0, v.size());

            // swap element i with it
            if (irand == (size_t) i)
                continue;
            ::swap (v[i], v[irand]);
        }
    }

    static void checkoverflow (size_t fieldval, size_t targetval, const char * fieldname)
    {
        if (fieldval != targetval)
            throw std::runtime_error (msra::strfun::strprintf ("checkoverflow: bit field %s too small for value 0x%x (cut from 0x%x)", fieldname, targetval, fieldval));
    }

    // helper for testing whether a swapped frame position is valid (w.r.t. beign in RAM when being at position 't')
    bool isframepositionvalid (const size_t t, const biggrowablevector<unsigned short> & ttochunk) const
    {
        // look up valid range for time position
        const size_t positionchunkindex = ttochunk[t];              // position 't' lies within this original chunk (relationship is monotonous, not random)
        const auto & chunk = randomizedchunks[positionchunkindex];
        // get in-RAM chunk range for this frame position (shared across all frame positions within the same chunk)
        const size_t poswindowbegin = chunk.windowbegin;            // rolling window over chunks (which under the hood have been randomized)
        const size_t poswindowend =   chunk.windowend;
        // Chunk implies that if we are at position 't', we are guaranteed to have chunks [poswindowbegin, poswindowend) in RAM.

        // now see if the randomized location is within that window
        const size_t actualchunkindexforpos = randomizedframerefs[t].chunkindex;    // where this frame pos has been mapped to
        return actualchunkindexforpos >= poswindowbegin && actualchunkindexforpos < poswindowend;
        // We only need to test the chunk index. Utterance and frame can be randomized within a chunk as we want, as long it is in RAM.
    }

    // big long helper to update all cached randomization information
    // This is a rather complex process since we randomize on two levels:
    //  - chunks of consecutive data in the feature archive
    //  - within a range of chunks that is paged into RAM
    //     - utterances (in utt mode), or
    //     - frames (in frame mode)
    // The 'globalts' parameter is the start time that triggered the rerandomization; it is NOT the base time of the randomized area.
    size_t lazyrandomization (const size_t globalts)
    {
        const size_t sweep = globalts / _totalframes;    // which sweep (this determines randomization)
        if (sweep == currentsweep)                       // already got this one--nothing to do
            return sweep;

        currentsweep = sweep;
        fprintf (stderr, "lazyrandomization: re-randomizing for sweep %zd in %s mode\n", currentsweep, framemode ? "frame" : "utterance");

        const size_t sweepts = sweep * _totalframes;     // first global frame index for this sweep

        // first randomize chunks
        std::vector<std::vector<utterancechunkdata>::const_iterator> randomizedchunkrefs;
        randomizedchunkrefs.reserve (allchunks.size());
        foreach_index (i, allchunks)    // TODO: this cries for iterating using the iterator!
            randomizedchunkrefs.push_back (allchunks.begin() + i);
        assert (randomizedchunkrefs.size() == allchunks.size());

        randomshuffle (randomizedchunkrefs, sweep); // bring into random order (with random seed depending on sweep)

        // place them onto the global timeline -> randomizedchunks[]
        // We are processing with randomization within a rolling window over this chunk sequence.
        // Paging will happen on a chunk-by-chunk basis.
        // The global time stamp is needed to determine the paging window.
        randomizedchunks.clear();               // data chunks after being brought into random order (we randomize within a rolling window over them)
        randomizedchunks.reserve (randomizedchunkrefs.size());
        foreach_index (k, randomizedchunkrefs)
            randomizedchunks.push_back (chunk (randomizedchunkrefs[k], randomizedchunks.empty() ? 0 : randomizedchunks.back().utteranceposend(), randomizedchunks.empty() ? sweepts : randomizedchunks.back().globalte()));
        assert (randomizedchunks.size() == allchunks.size());
        assert (randomizedchunks.empty() || (randomizedchunks.back().utteranceposend() == numutterances && randomizedchunks.back().globalte() == sweepts + _totalframes));

        // for each chunk, compute the randomization range (w.r.t. the randomized chunk sequence)
        foreach_index (k, randomizedchunks)
        {
            chunk & chunk = randomizedchunks[k];
            // start with the range of left neighbor
            if (k == 0)
            {
                chunk.windowbegin = 0;
                chunk.windowend = 1;
            }
            else
            {
                chunk.windowbegin = randomizedchunks[k-1].windowbegin;  // might be too early
                chunk.windowend = randomizedchunks[k-1].windowend;      // might have more space
            }
            while (chunk.globalts - randomizedchunks[chunk.windowbegin].globalts > randomizationrange/2)
                chunk.windowbegin++;            // too early
            while (chunk.windowend < randomizedchunks.size() && randomizedchunks[chunk.windowend].globalte() - chunk.globalts < randomizationrange/2)
                chunk.windowend++;              // got more space
        }

        if (!framemode)     // utterance mode
        {
            // This sets up the following members:
            //  - positionchunkwindows
            //  - randomizedutterancerefs
            //  - randomizedutteranceposmap

            // We will now introduce the concept of utterance *position*.
            // During processing, utterances will be indexed by position (which is in turn derived from a frame index in getbatch()),
            // and it is assumed (required) that positions are requested consecutively.
            // Each utterance position has an underlying associated utterance, which is represented as (chunkid, within-chunk index) and randomly assigned.
            // Each utterance position also has an associated range of chunks that are kept in memory,
            // and the associated underlying utterance is guaranteed to be found within that associated range of chunks.
            // That allows to page out/in data when processing utterance positions in a consecutive manner.

            // compute chunk windows for every utterance position -> positionchunkwindows[]
            // Utterance positions can only reference underlying utterance data within the chunk window.
            // Utterance positions are defined by the randomized chunk sequence (i.e. their underlying 'defining' chunk differs from sweep to sweep).
            positionchunkwindows.clear();           // [utterance position] -> [windowbegin, windowend) for controlling paging
            positionchunkwindows.reserve (numutterances);
            foreach_index (k, randomizedchunks) // TODO: this really cries for iterating using iterators!
            {
                chunk & chunk = randomizedchunks[k];
                for (size_t i = chunk.utteranceposbegin; i < chunk.utteranceposend(); i++)  // loop over utterances in this chunk
                    positionchunkwindows.push_back (randomizedchunks.begin() + k);
                // to look up the chunk range in memory for a position, look up the defining chunk and its range
            }
            assert (positionchunkwindows.size() == numutterances);

            // build the randomized utterances array -> randomizedutterancerefs[]
            // start by assigning all utterance positions to utterances in non-random consecutive manner
            randomizedutterancerefs.clear();        // [pos] randomized utterance ids
            randomizedutterancerefs.reserve (numutterances);
            foreach_index (k, randomizedchunks)
            {
                chunk & chunk = randomizedchunks[k];
                for (size_t i = 0; i < chunk.numutterances(); i++)  // loop over utterances in this chunk
                    randomizedutterancerefs.push_back (utteranceref (k, i));
            }
            assert (randomizedutterancerefs.size() == numutterances);
            foreach_index (i, randomizedutterancerefs)
            {
                auto & uttref = randomizedutterancerefs[i];
                assert (positionchunkwindows[i].isvalidforthisposition (uttref)); uttref;
            }

            // check we got those setup right

            // we now randomly shuffle randomizedutterancerefs[pos], while considering the constraints of what chunk range needs to be in memory
            srand ((unsigned int) sweep + 1);
            for (size_t i = 0; i < randomizedutterancerefs.size(); i++)
            {
                // get valid randomization range, expressed in chunks
                const size_t windowbegin = positionchunkwindows[i].windowbegin();
                const size_t windowend =   positionchunkwindows[i].windowend();

                // get valid randomization range, expressed in utterance positions
                // Remember, utterance positions are defined by chunks.
                const size_t posbegin = randomizedchunks[windowbegin].utteranceposbegin;
                const size_t posend =   randomizedchunks[windowend-1].utteranceposend();

                // randomization range for this utterance position is [posbegin, posend)
                for(;;)
                {
                    // pick a random location
                    const size_t j = msra::dbn::rand (posbegin, posend);    // a random number within the window
                    if (i == j)
                        break;  // the random gods say "this one points to its original position"... nothing wrong about that, but better not try to swap

                    // We want to swap utterances at i and j, but need to make sure they remain in their allowed range.
                    // This is guaranteed for a so-far untouched utterance, but both i and j may have been touched by a previous swap.

                    // We want to use the utterance previously referenced at utterance position j at position i. Is that allowed?
                    if (!positionchunkwindows[i].isvalidforthisposition (randomizedutterancerefs[j]))
                        continue;   // nope --try another

                    // Likewise may we use the utterance previously referenced at utterance position i at position j?
                    if (!positionchunkwindows[j].isvalidforthisposition (randomizedutterancerefs[i]))
                        continue;   // nope --try another

                    // yep--swap them
                    randomizedutterancerefs[i].swap (randomizedutterancerefs[j]);
                    break;
                }
            }

            // place the randomized utterances on the global timeline so we can find them by globalts
            size_t t = sweepts;
            foreach_index (i, randomizedutterancerefs)
            {
                auto & uttref = randomizedutterancerefs[i];
                uttref.globalts = t;
                uttref.numframes = randomizedchunks[uttref.chunkindex].getchunkdata().numframes (uttref.utteranceindex);
                t = uttref.globalte();
            }
            assert (t == sweepts + _totalframes);

            // verify that we got it right (I got a knot in my head!)
            foreach_index (i, randomizedutterancerefs)
            {
                // get utterance referenced at this position
                const auto & uttref = randomizedutterancerefs[i];
                // check if it is valid for this position
                if (uttref.chunkindex < positionchunkwindows[i].windowbegin() || uttref.chunkindex >= positionchunkwindows[i].windowend())
                    throw std::logic_error ("lazyrandomization: randomization logic mangled!");
            }

            // create lookup table for (globalts values -> pos) -> randomizedutteranceposmap[]
            randomizedutteranceposmap.clear();      // [globalts] -> pos lookup table
            foreach_index (pos, randomizedutterancerefs)
            {
                auto & uttref = randomizedutterancerefs[pos];
                randomizedutteranceposmap[uttref.globalts] = (size_t) pos;
            }
        }
        else            // frame mode
        {
            // This sets up the following members:
            //  - randomizedframerefs

            srand ((unsigned int) sweep + 1);
            // An original timeline is established by the randomized chunks, denoted by 't'.
            // Returned frames are indexed by frame position j = (globalt - sweept), which have an associated underlying 't'.
            // It is guaranteed that uttterance frame position j maps to an underlying frame within the corresponding chunk window.
            biggrowablevector<unsigned short> ttochunk; // randomized chunk index associated with frame position
            ttochunk.resize (_totalframes);
            size_t t = 0;
            frameref frameref;
            // enumerate chunks in their randomized order and assign frame indices in that order -> randomizedframerefs[t]
            // At this point, chunks are in randomized order, but utterances and frames within utterances are not randomized.
            // Later we will randomize those as well.
            foreach_index (i, randomizedchunks)
            {
                frameref.chunkindex = (unsigned short)i;
                checkoverflow (frameref.chunkindex, i, "frameref::chunkindex");
                const auto & chunk = randomizedchunks[i];
                const auto & chunkdata = chunk.getchunkdata();
                const size_t numutt = chunkdata.numutterances();
                for (size_t k = 0; k < numutt; k++)
                {
                    frameref.utteranceindex = (short)k;
                    checkoverflow (frameref.utteranceindex, k, "frameref::utteranceindex");
                    const size_t n = chunkdata.numframes (k);
                    for (size_t m = 0; m < n; m++)
                    {
                        frameref.frameindex = (short)m;
                        checkoverflow (frameref.frameindex, m, "frameref::utteranceindex");
                        randomizedframerefs[t] = frameref;  // hopefully this is a memory copy, not a bit-wise assignment! If not, then code it explicitly
                        ttochunk[t] = (unsigned short) i;
                        checkoverflow (ttochunk[t], i, "ttochunk[]");
                        t++;
                    }
                }
            }
            assert (t == _totalframes);

            // now randomize them --we use the nested loop again to avoid storing a backpointer
            // The condition is that a randomized frame may not be moved out of its associated chunk window.
            foreach_index (t, randomizedframerefs)
            {
                const size_t positionchunkindex = ttochunk[t];              // position 't' lies within this chunk (relationship is monotonous, not random)
                const auto & chunk = randomizedchunks[positionchunkindex];  // for window

                // get in-RAM chunk range for this frame position (shared across all frame positions within the same chunk)
                const size_t poswindowbegin = chunk.windowbegin;            // rolling window over chunks (which under the hood have been randomized)
                const size_t poswindowend =   chunk.windowend;
                // Chunk implies that if we are at position 't', we are guaranteed to have chunks [poswindowbegin, poswindowend) in RAM.
                // These chunks are associated with a range of frame positions.
                // It is implied that if we are at position 't', the frames covered by chunks [poswindowbegin, poswindowend) are in RAM.
                const size_t postbegin = randomizedchunks[poswindowbegin].globalts   - sweepts;
                const size_t postend =   randomizedchunks[poswindowend-1].globalte() - sweepts;
                // The position that this frame gets randomized to must be guaranteed to belong to a chunk within [postbegin, postend).

                for (;;)                                                    // (randomization retry loop)
                {
                    size_t tswap = msra::dbn::rand (postbegin, postend);               // random frame position within allowed range
                    // We want to swap 't' to 'tswap' and 'tswap' to 't'.
                    //  - Both may have been swapped before.
                    //  - Both must stay within the randomization window of their respective position.
                    // check admissibility of where the element at 'tswap' gets swapped to 't' (range = [windowbegin,windowend))
                    size_t tswapchunkindex = randomizedframerefs[tswap].chunkindex;
                    if (tswapchunkindex < poswindowbegin || tswapchunkindex >= poswindowend)
                        continue;
                    // check admissibility of where the element at t gets swapped to (which is frame position 'tswap')
                    const size_t sourcechunkindex = randomizedframerefs[t].chunkindex;
                    size_t targetchunkindex = ttochunk[tswap];      // chunk associated with this frame position defines value range
                    const auto & targetchunk = randomizedchunks[targetchunkindex];
                    const size_t targetwindowbegin = targetchunk.windowbegin;
                    const size_t targetwindowend =   targetchunk.windowend;
                    if (sourcechunkindex < targetwindowbegin || sourcechunkindex >= targetwindowend)
                        continue;
                    // admissible--swap the two
                    ::swap (randomizedframerefs[t], randomizedframerefs[tswap]);
#if 0
                    break;
#else               // post-check  --so far did not trigger, can be removed

                    // do a post-check if we got it right  --we seem not to
                    if (isframepositionvalid (t, ttochunk) && isframepositionvalid (tswap, ttochunk))
                        break;
                    // not valid: swap them back and try again  --we actually discovered a bug in the code above
                    ::swap (randomizedframerefs[t], randomizedframerefs[tswap]);
                    fprintf (stderr, "lazyrandomization: BUGBUG --invalid swapping condition detected\n");
#endif
                }
            }

            // check it --my head spins
            t = 0;
            foreach_index (i, randomizedchunks)
            {
                const auto & chunk = randomizedchunks[i];       // for window and chunkdata
                const size_t poswindowbegin = chunk.windowbegin;
                const size_t poswindowend =   chunk.windowend;

                const auto & chunkdata = chunk.getchunkdata();  // for numutterances/numframes
                const size_t numutt = chunkdata.numutterances();
                for (size_t k = 0; k < numutt; k++)
                {
                    const size_t n = chunkdata.numframes (k);
                    for (size_t m = 0; m < n; m++)
                    {
                        const size_t randomizedchunkindex = randomizedframerefs[t].chunkindex;
                        if (randomizedchunkindex < poswindowbegin || randomizedchunkindex >= poswindowend)
                            throw std::logic_error ("lazyrandomization: nope, you got frame randomization wrong, dude");
                        t++;
                    }
                }
            }
            assert (t == _totalframes);
        }

        return sweep;
    }

    // helper to page out a chunk with log message
    void releaserandomizedchunk (size_t k)
    {
        auto & chunkdata = randomizedchunks[k].getchunkdata();
        if (!chunkdata.isinram())
            return;       // already out

        fprintf (stderr, "releaserandomizedchunk: paging out randomized chunk %zd (frame range [%zd..%zd]), %zd resident in RAM\n",
                 k, randomizedchunks[k].globalts, randomizedchunks[k].globalte()-1, chunksinram-1);
        chunkdata.releasedata();
        chunksinram--;
    }

    // helper to page in a chunk for a given utterance
    // (window range passed in for checking only)
    // Returns true if we actually did read something.
    bool requirerandomizedchunk (const size_t chunkindex, const size_t windowbegin, const size_t windowend)
    {
        if (chunkindex < windowbegin || chunkindex >= windowend)
            throw std::logic_error ("requirerandomizedchunk: requested utterance outside in-memory chunk range");

        auto & chunk = randomizedchunks[chunkindex];
        auto & chunkdata = chunk.getchunkdata();
        if (chunkdata.isinram())
            return false;

        fprintf (stderr, "requirerandomizedchunk: paging in randomized chunk %zd (frame range [%zd..%zd]), %zd resident in RAM\n", chunkindex, chunk.globalts, chunk.globalte()-1, chunksinram+1);
        msra::util::attempt (5, [&]()   // (reading from network)
        {
            chunkdata.requiredata (featkind, featdim, sampperiod, this->lattices);
        });
        chunksinram++;
        return true;
    }

    class matrixasvectorofvectors  // wrapper around a matrix that views it as a vector of column vectors
    {
        void operator= (const matrixasvectorofvectors &);  // non-assignable
        msra::dbn::matrixbase & m;
    public:
        matrixasvectorofvectors (msra::dbn::matrixbase & m) : m (m) {}
        size_t size() const { return m.cols(); }
        const_array_ref<float> operator[] (size_t j) const { return array_ref<float> (&m(0,j), m.rows()); }
    };

    size_t chunkforframepos (const size_t t) const  // find chunk for a given frame position
    {
        auto iter = std::lower_bound (randomizedchunks.begin(), randomizedchunks.end(), t, [&] (const chunk & chunk, size_t t) { return chunk.globalte() <= t; });
        const size_t chunkindex = iter - randomizedchunks.begin();
        if (t < randomizedchunks[chunkindex].globalts || t >= randomizedchunks[chunkindex].globalte())
            throw std::logic_error ("chunkforframepos: dude, learn STL!");
        return chunkindex;
    }

public:

    void setverbosity(int newverbosity){ verbosity = newverbosity; }

    // get the next minibatch
    // A minibatch is made up of one or more utterances.
    // We will return less than 'framesrequested' unless the first utterance is too long.
    // Note that this may return frames that are beyond the epoch end, but the first frame is always within the epoch.
    // We specify the utterance by its global start time (in a space of a infinitely repeated training set).
    // This is efficient since getbatch() is called with sequential 'globalts' except at epoch start.
    // Note that the start of an epoch does not necessarily fall onto an utterance boundary. The caller must use firstvalidglobalts() to find the first valid globalts at or after a given time.
    /*implement*/ bool getbatch (const size_t globalts, const size_t framesrequested, msra::dbn::matrix & feat, std::vector<size_t> & uids,
                                 std::vector<const_array_ref<msra::lattices::lattice::htkmlfwordsequence::word>> & transcripts, std::vector<shared_ptr<const latticesource::latticepair>> & latticepairs)
    {
        bool readfromdisk = false;  // return value: shall be 'true' if we paged in anything

        auto_timer timergetbatch;
        assert (_totalframes > 0);

        // update randomization if a new sweep is entered  --this is a complex operation that updates many of the data members used below
        const size_t sweep = lazyrandomization (globalts);

        const std::vector<char> noboundaryflags;    // dummy
        if (!framemode)      // regular utterance mode
        {
            // find utterance position for globalts
            // There must be a precise match; it is not possible to specify frames that are not on boundaries.
            auto positer = randomizedutteranceposmap.find (globalts);
            if (positer == randomizedutteranceposmap.end())
                throw std::logic_error ("getbatch: invalid 'globalts' parameter; must match an existing utterance boundary");
            const size_t spos = positer->second;

            // determine how many utterances will fit into the requested minibatch size
            size_t mbframes = randomizedutterancerefs[spos].numframes;   // at least one utterance, even if too long
            size_t epos;
            for (epos = spos + 1; epos < numutterances && mbframes + randomizedutterancerefs[epos].numframes < framesrequested; epos++)  // add more utterances as long as they fit within requested minibatch size
                mbframes += randomizedutterancerefs[epos].numframes;

            // do some paging housekeeping
            // This will also set the feature-kind information if it's the first time.
            // Free all chunks left of the range.
            // Page-in all chunks right of the range.
            // We are a little more blunt for now: Free all outside the range, and page in only what is touched. We could save some loop iterations.
            const size_t windowbegin = positionchunkwindows[spos].windowbegin();
            const size_t windowend =   positionchunkwindows[epos-1].windowend();
            for (size_t k = 0; k < windowbegin; k++)
                releaserandomizedchunk (k);
            for (size_t k = windowend; k < randomizedchunks.size(); k++)
                releaserandomizedchunk (k);
            for (size_t pos = spos; pos < epos; pos++)
                readfromdisk |= requirerandomizedchunk (randomizedutterancerefs[pos].chunkindex, windowbegin, windowend); // (window range passed in for checking only)

            // resize feat and uids
            feat.resize (vdim, mbframes);
            if (issupervised())             // empty means unsupervised training -> return empty uids
                uids.resize (mbframes);
            else
                uids.clear();
            latticepairs.clear();               // will push_back() below
            transcripts.clear();

            // return these utterances
            fprintf (stderr, "getbatch: getting utterances %zd..%zd (%zd frames out of %zd requested) in sweep %zd\n", spos, epos -1, mbframes, framesrequested, sweep);
            size_t tspos = 0;   // relative start of utterance 'pos' within the returned minibatch
            for (size_t pos = spos; pos < epos; pos++)
            {
                const auto & uttref = randomizedutterancerefs[pos];
                const auto & chunk = randomizedchunks[uttref.chunkindex];
                const auto & chunkdata = chunk.getchunkdata();
                assert (uttref.globalts == globalts + tspos);
                auto uttframes = chunkdata.getutteranceframes (uttref.utteranceindex);
                matrixasvectorofvectors uttframevectors (uttframes);    // (wrapper that allows m[j].size() and m[j][i] as required by augmentneighbors())
                const size_t n = uttframevectors.size();
                assert (n == uttframes.cols() && uttref.numframes == n && chunkdata.numframes (uttref.utteranceindex) == n);
                /*
                // copy the frames and class labels
                size_t leftextent, rightextent;
                // page in the needed range of frames
                if (leftcontext == 0 && rightcontext == 0)
                {
                    leftextent = rightextent = augmentationextent(feat.col(0).size(), vdim);
                }
                else
                {
                    leftextent = leftcontext;
                    rightextent = rightcontext;
                }
                */
                auto uttclassids = getclassids (uttref);
                for (size_t t = 0; t < n; t++)          // t = time index into source utterance
                {
                    augmentneighbors (uttframevectors, noboundaryflags, t, feat, t + tspos);
                    if (issupervised())
                        uids[t + tspos] = uttclassids[t];
                }

                if (!this->lattices.empty())
                {
                    auto latticepair = chunkdata.getutterancelattice (uttref.utteranceindex);
                    latticepairs.push_back (latticepair);
                    // look up reference
                    const auto & key = latticepair->getkey();
                    if (!allwordtranscripts.empty())
                    {
                        const auto & transcript = allwordtranscripts.find (key)->second;
                        transcripts.push_back (transcript.words);
                    }
                }

                tspos += n;
            }
            assert (tspos == mbframes);
        }
        else                // // debug mode returning randomized frames again, to see whether convergence is better (we don't ensure non-repetition at this point)
        {
            const size_t sweepts = sweep * _totalframes;         // first global frame index for this sweep
            const size_t sweepte = sweepts + _totalframes;       // and its end
            const size_t globalte = min (globalts + framesrequested, sweepte);  // we return as much as requested, but not exceeding sweep end
            const size_t mbframes = globalte - globalts;        // that's our mb size

            // determine window range
            // We enumerate all frames--can this be done more efficiently?
            const size_t firstchunk = chunkforframepos (globalts);
            const size_t lastchunk = chunkforframepos (globalte-1);
            const size_t windowbegin = randomizedchunks[firstchunk].windowbegin;
            const size_t windowend = randomizedchunks[lastchunk].windowend;
            fprintf (stderr, "getbatch: getting randomized frames [%zd..%zd] (%zd frames out of %zd requested) in sweep %zd; chunks [%zd..%zd] -> chunk window [%zd..%zd)\n",
                     globalts, globalte, mbframes, framesrequested, sweep, firstchunk, lastchunk, windowbegin, windowend);
            // release all data outside, and page in all data inside
            for (size_t k = 0; k < windowbegin; k++)
                releaserandomizedchunk (k);
            for (size_t k = windowbegin; k < windowend; k++)
                readfromdisk |= requirerandomizedchunk (k, windowbegin, windowend); // (window range passed in for checking only, redundant here)
            for (size_t k = windowend; k < randomizedchunks.size(); k++)
                releaserandomizedchunk (k);

            // resize feat and uids
            feat.resize (vdim, mbframes);
            if (issupervised())             // empty means unsupervised training -> return empty uids
                uids.resize (mbframes);
            else
                uids.clear();

            // return randomized frames for the time range of those utterances
            for (size_t j = 0; j < mbframes; j++)
            {
                // map to time index inside arrays
                const size_t framepos = (globalts + j) % _totalframes;  // using mod because we may actually run beyond the sweep for the last call
                const frameref & frameref = randomizedframerefs[framepos];

                // random utterance
                readfromdisk |= requirerandomizedchunk (frameref.chunkindex, windowbegin, windowend);    // (this is just a check; should not actually page in anything)
                const auto & chunk = randomizedchunks[frameref.chunkindex];
                const auto & chunkdata = chunk.getchunkdata();
                auto uttframes = chunkdata.getutteranceframes (frameref.utteranceindex);
                matrixasvectorofvectors uttframevectors (uttframes);    // (wrapper that allows m[j].size() and m[j][i] as required by augmentneighbors())
                const size_t n = uttframevectors.size();
                assert (n == uttframes.cols() && chunkdata.numframes (frameref.utteranceindex) == n); n;

                // copy frame and class labels
                const size_t t = frameref.frameindex;
                /*
                size_t leftextent, rightextent;
                // page in the needed range of frames
                if (leftcontext == 0 && rightcontext == 0)
                {
                    leftextent = rightextent = augmentationextent(feat.col(0).size(), vdim);
                }
                else
                {
                    leftextent = leftcontext;
                    rightextent = rightcontext;
                }
                */
                augmentneighbors (uttframevectors, noboundaryflags, t, feat, j);
                if (issupervised())
                    uids[j] = getclassids (frameref)[t];
            }
        }
        timegetbatch = timergetbatch;
        return readfromdisk;
    }

    bool getbatch (const size_t globalts, const size_t framesrequested, std::vector<msra::dbn::matrix> & feat, std::vector<std::vector<size_t>> & uids,
            std::vector<std::vector<const_array_ref<msra::lattices::lattice::htkmlfwordsequence::word>>> & transcripts, 
            std::vector<std::vector<shared_ptr<const latticesource::latticepair>>> & latticepairs)
    {
        // for single input/output set size to be 1 and run old getbatch
        feat.resize(1);
        uids.resize(1);
        transcripts.resize(1);
        latticepairs.resize(1);
        return getbatch(globalts, framesrequested, feat[0], uids[0], transcripts[0], latticepairs[0]);
    }

    double gettimegetbatch() { return timegetbatch;}
    // alternate (updated) definition for multiple inputs/outputs - read as a vector of feature matrixes or a vector of label strings
    /*implement*/ bool getbatch (const size_t globalts,
                           const size_t framesrequested, std::vector<msra::dbn::matrix> & feat, std::vector<std::vector<size_t>> & uids,
                           std::vector<const_array_ref<msra::lattices::lattice::htkmlfwordsequence::word>> & transcripts,
                           std::vector<shared_ptr<const latticesource::latticepair>> & latticepairs)
    {
        // for single input/output set size to be 1 and run old getbatch
        feat.resize(1);
        uids.resize(1);
        return getbatch(globalts, framesrequested, feat[0], uids[0], transcripts, latticepairs);
    }

    size_t totalframes() const { return _totalframes; }

    // return first valid globalts to ask getbatch() for
    // In utterance mode, the epoch start may fall in the middle of an utterance.
    // We return the end time of that utterance (which, in pathological cases, may in turn be outside the epoch; handle that).
    /*implement*/ size_t firstvalidglobalts (const size_t globalts)
    {
        // update randomization if a new sweep is entered  --this is a complex operation that updates many of the data members used below
        const size_t sweep = lazyrandomization (globalts);
        // frame mode: start at sweep boundary directly
        if (framemode)
            return globalts;
        // utterance mode
        assert (globalts >= sweep * _totalframes && globalts < (sweep + 1) * _totalframes); sweep;
        foreach_index (pos, randomizedutterancerefs)
            if (randomizedutterancerefs[pos].globalts >= globalts)
                return randomizedutterancerefs[pos].globalts;   // exact or inexact match
        return randomizedutterancerefs.back().globalte();       // boundary case: requested time falls within the last utterance
    }

    /*implement*/ const std::vector<size_t> & unitcounts() const { return counts; }
};

};};
