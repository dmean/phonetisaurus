/*
 phonetisaurus-align.cpp

 Copyright (c) [2012-], Josef Robert Novak
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
  modification, are permitted #provided that the following conditions
  are met:

  * Redistributions of source code must retain the above copyright 
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above 
    copyright notice, this list of #conditions and the following 
    disclaimer in the documentation and/or other materials provided 
    with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
 "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE 
 COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, 
 INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, 
 STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED 
 OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/
#include "M2MFstAligner.hpp"
#include "LatticePruner.hpp"
#include "FstPathFinder.hpp"
#include "util.hpp"
using namespace fst;


void load_input_file( M2MFstAligner* aligner, string input_file, string delim, string s1_char_delim, string s2_char_delim ){
  ifstream infile( input_file.c_str() );
  string line;
  cerr << "Loading input file: " << input_file << endl;

  if( infile.is_open() ){
    while( infile.good() ){
      getline(infile, line);
      if( line.empty() )
        continue;
      vector<string> tokens = tokenize_utf8_string( &line, &delim );
      vector<string> seq1   = tokenize_utf8_string( &tokens.at(0), &s1_char_delim );
      vector<string> seq2   = tokenize_utf8_string( &tokens.at(1), &s2_char_delim );
      aligner->entry2alignfst( seq1, seq2 );
    }
    infile.close();
  }
  //aligner->computePenalties( );

  return;
}

void write_alignments( M2MFstAligner* aligner, string ofile_name, StdArc::Weight threshold, int nbest, bool fb, bool penalize ){
  /* 
     Write the raw alignments to a file in text-based corpus format. 

     NOTE: Although N-best and other pruning strategies are supported,
           the final format is that of a standard text corpus.  All relative
	   token and pronunciation scores will be stripped.  In general
	   this means that, unless you are very lucky with your combined 
	   pruning strategy the un-ranked N-best hypotheses will result in a 
	   lower-quality joint N-gram model.

	   This approach is best used with simple 1-best.
  */

  //Build us a lattice pruner
  LatticePruner pruner( aligner->penalties, threshold, nbest, fb, penalize );

  ofstream ofile(ofile_name.c_str());
  for( int i=0; i<aligner->fsas.size(); i++ ){
    //Map to Tropical semiring
    VectorFst<StdArc>* tfst = new VectorFst<StdArc>();
    Map(aligner->fsas.at(i), tfst, LogToStdMapper());
    pruner.prune_fst( tfst );
    RmEpsilon( tfst );
    //Skip empty results.  This should only happen
    // in the following situations:
    //  1. seq1_del=false && len(seq1)<len(seq2) 
    //  2. seq2_del=false && len(seq1)>len(seq2)
    //In both 1.and 2. the issue is that we need to 
    // insert a 'skip' in order to guarantee at least
    // one valid alignment path through seq1*seq2, but
    // user params didn't allow us to.  
    //Probably better to insert these where necessary
    // during initialization, regardless of user prefs.
    vector<PathData> paths;
    if( tfst->NumStates()>0 ){
      FstPathFinder pathfinder( aligner->skipSeqs );
      pathfinder.isyms = aligner->isyms;
      pathfinder.findAllStrings( *tfst );
      paths = pathfinder.paths;
    }

    for( int j=0; j<paths.size(); j++ ){
	for( int k=0; k<paths[j].path.size(); k++ ){
	  if( ofile )
	    ofile << paths[j].path[k];
	  else
	    cout << paths[j].path[k];
	  if( j<paths[j].path.size() ){
	    if( ofile )
	      ofile << " ";
	    else
	      cout << " ";
	  }
	}
	if( ofile )
	  ofile << endl;
	else
	  cout << endl;
    }
    delete tfst;
  }

  return;
}

void compileNBestFarArchive( M2MFstAligner* aligner, vector<VectorFst<LogArc> > *fsts, string far_name, 
			     StdArc::Weight threshold, int nbest, bool fb, bool penalize ){
  /*
    Generic method for compiling an FstARchive from a vector of FST lattices.
    The 'nbest' and 'threshold' parameters may be used to heuristically prune
    the individual input lattices.  

    TODO: Forward-Backward pruning for the lattices for one last variation.
  */

  //Book-keeping stuff
  string key_prefix = "";
  string key_suffix = "";
  string key        = "";
  char   keybuf[16];
  int32  generate_keys = 7; //Suitable for up to several million lattices

  //Build us a FarWriter to compile the archive
  FarWriter<StdArc> *far_writer = FarWriter<StdArc>::Create(far_name, FAR_DEFAULT);
  //Build us a lattice pruner
  LatticePruner pruner( aligner->penalties, threshold, nbest, fb, penalize );

  for( int i=0; i < fsts->size(); i++ ){
    //There has got to be a more efficient way to do this!
    VectorFst<StdArc>* tfst = new VectorFst<StdArc>();
    VectorFst<LogArc>* lfst = new VectorFst<LogArc>();
    VectorFst<LogArc>* pfst = new VectorFst<LogArc>();
    VectorFst<StdArc>* ffst = new VectorFst<StdArc>();

    //Map to the Tropical semiring
    Map(fsts->at(i), tfst, LogToStdMapper());
    pruner.prune_fst( tfst );

    //Map back to the Log semiring
    Map(*tfst, lfst, StdToLogMapper());

    //Perform posterior normalization of the N-best lattice by pushing weights 
    //  in the log semiring and then removing the final weight.
    //When N=1, this will also have the effect of removing all weights.
    //  The .far result here will be identical to the non-lattice 'write_alignments()'.
    Push<LogArc, REWEIGHT_TO_FINAL>(*lfst, pfst, kPushWeights);
    for( StateIterator<VectorFst<LogArc> > siter(*pfst); !siter.Done(); siter.Next() ){
      size_t i = siter.Value();
      if( pfst->Final(i)!=LogArc::Weight::Zero() ){
        pfst->SetFinal(i,LogArc::Weight::One());
      }
    }

    //Finally map back to the Tropical semiring for the last time
    Map(*pfst, ffst, LogToStdMapper());

    sprintf(keybuf, "%0*d", generate_keys, i+1);
    key = keybuf;
    //Write the final result to the FARchive
    far_writer->Add(key_prefix + key + key_suffix, *ffst);
    //Cleanup the temporary FSTs
    delete lfst, tfst, pfst, ffst;
  }
  //Cleanup the archive writer
  delete far_writer;

  return;
}


DEFINE_string( input,          "",  "Two-column input file to align." );
DEFINE_bool(   seq1_del,     true,  "Allow deletions in sequence one." );
DEFINE_bool(   seq2_del,     true,  "Allow deletions in sequence two." );
DEFINE_int32(  seq1_max,        2,  "Maximum subsequence length for sequence one." );
DEFINE_int32(  seq2_max,        2,  "Maximum subsequence length for sequence two." );
DEFINE_string( seq1_sep,      "|",  "Multi-token separator for input tokens." );
DEFINE_string( seq2_sep,      "|",  "Multi-token separator for output tokens." );
DEFINE_string( s1s2_sep,      "}",  "Token used to separate input-output subsequences in the g2p model." );
DEFINE_string( delim,        "\t",  "Delimiter separating entry one and entry two in the input file." );
DEFINE_string( eps,       "<eps>",  "Epsilon symbol." );
DEFINE_string( skip,          "_",  "Skip token used to represent null transitions.  Distinct from epsilon." );
DEFINE_bool(   penalize,     true,  "Penalize scores." );
DEFINE_bool(   penalize_em, false,  "Penalize links during EM training." );
DEFINE_bool(   load_model,          "",  "Load a pre-trained model for use." );
DEFINE_string( ofile,          "",  "Output file to write the aligned dictionary to." );
DEFINE_bool(   mbr,         false,  "Use the LMBR decoder (not yet implemented)." );
DEFINE_bool(   fb,          false,  "Use forward-backward pruning for the alignment lattices." );
DEFINE_int32(  iter,           11,  "Maximum number of EM iterations to perform." );
DEFINE_double( thresh,      1e-10,  "Delta threshold for EM training termination." ); 
DEFINE_int32(  nbest,           1,  "Output the N-best alignments given the model." );
DEFINE_double( pthresh,       -99,  "Pruning threshold.  Use to prune unlikely N-best candidates when using multiple alignments.");
DEFINE_string( s1_char_delim,  "",  "Sequence one input delimeter." );
DEFINE_string( s2_char_delim, " ",  "Sequence two input delimeter." );
DEFINE_string( write_model,    "",  "Write out the alignment model in OpenFst format to filename." );
DEFINE_bool(   lattice,     false,  "Write out the alignment lattices as an fst archive (.far)." );
DEFINE_bool(   restrict,     true,  "Restrict links to M-1, 1-N during initialization." );
int main( int argc, char* argv[] ){
  string usage = "phonetisaurus-align dictionary aligner.\n\n Usage: ";
  set_new_handler(FailedNewHandler);
  SetFlags(usage.c_str(), &argc, &argv, false );

  M2MFstAligner aligner( 
			FLAGS_seq1_del, FLAGS_seq2_del, FLAGS_seq1_max, FLAGS_seq2_max, 
			FLAGS_seq1_sep, FLAGS_seq2_sep, FLAGS_s1s2_sep, 
			FLAGS_eps, FLAGS_skip, FLAGS_penalize, FLAGS_penalize_em, FLAGS_restrict
			 );

  load_input_file( &aligner, FLAGS_input, FLAGS_delim, FLAGS_s1_char_delim, FLAGS_s2_char_delim );

  cerr << "Starting EM..." << endl;
  aligner.maximization(false);
  cerr << "Finished first iter..." << endl;
  for( int i=1; i<=FLAGS_iter; i++ ){
    cerr << "Iteration: " << i << " Change: ";
    aligner.expectation();
    cerr << aligner.maximization(false) << endl;
  }

  cerr << "Last iteration: " << endl;
  aligner.expectation();
  aligner.maximization(true);

  StdArc::Weight pthresh = FLAGS_pthresh==-99.0 ? LogWeight::Zero().Value() : FLAGS_pthresh;
  aligner.fsas[0].SetInputSymbols(aligner.isyms);
  aligner.fsas[0].SetOutputSymbols(aligner.isyms);
  if( FLAGS_write_model.compare("") != 0 ){
    cerr << "Writing alignment model in OpenFst format to file: " << FLAGS_write_model << endl;
  }
  if( FLAGS_lattice==true )
    compileNBestFarArchive( &aligner, &aligner.fsas, FLAGS_ofile, pthresh, FLAGS_nbest, FLAGS_fb, FLAGS_penalize );
  else
    write_alignments( &aligner, FLAGS_ofile, pthresh, FLAGS_nbest, FLAGS_fb, FLAGS_penalize );

  return 1;
}
