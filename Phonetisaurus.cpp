/*
 *  Phonetisaurus.cpp 
 *  
 *  Created by Josef Novak on 2011-04-07.
 *  Copyright 2011 Josef Novak. All rights reserved.
 *
 */
#include <stdio.h>
#include <string>
#include <fst/fstlib.h>
#include <iostream>
#include <set>
#include <algorithm>
#include "FstPathFinder.hpp"
#include "Phonetisaurus.hpp"

using namespace fst;

Phonetisaurus::Phonetisaurus( ) {
    //Default constructor
}

Phonetisaurus::Phonetisaurus( const char* g2pmodel_file ) {
    //Base constructor.  Load the clusters file, the models and setup shop.
    eps  = "<eps>";
    sb   = "<s>";
    se   = "</s>";
    skip = "_";
    tie  = "|";
    
    skipSeqs.insert(eps);
    skipSeqs.insert(sb);
    skipSeqs.insert(se);
    skipSeqs.insert(skip);
    skipSeqs.insert("-");
    
    g2pmodel = StdVectorFst::Read( g2pmodel_file );

    isyms = (SymbolTable*)g2pmodel->InputSymbols(); 

    osyms = (SymbolTable*)g2pmodel->OutputSymbols(); 
    
    loadClusters( );
    
    epsMapper = makeEpsMapper( );
    
    //We need make sure the g2pmodel is arcsorted
    ILabelCompare<StdArc> icomp;
    ArcSort( g2pmodel, icomp );
}


void Phonetisaurus::loadClusters( ){
    /*
     Load the clusters file containing the list of 
     subsequences generated during multiple-to-multiple alignment
    */
    
    for( size_t i = 0; i < isyms->NumSymbols(); i++ ){
        string sym = isyms->Find( i );
        
        if( sym.find(tie) != string::npos ){
            char* tmpstring = (char *)sym.c_str();
            char* p = strtok(tmpstring, tie.c_str());
            vector<string> cluster;
            
            while (p) {
                cluster.push_back(p);
                p = strtok(NULL, tie.c_str());
            }
            
            clusters[cluster] = i;
        }
    }
    return;
}

StdVectorFst Phonetisaurus::makeEpsMapper( ){
    /*
     Generate a mapper FST to transform unwanted output symbols
     to the epsilon symbol.
     
     This can be used to remove unwanted symbols from the final 
     result, but in tests was 7x slower than manual removal
     via the FstPathFinder object.
     */
    
    StdVectorFst mfst;
    mfst.AddState();
    mfst.SetStart(0);
    
    set<string>::iterator sit;
    for( size_t i=0; i< osyms->NumSymbols(); i++ ){
        string sym = osyms->Find( i );
        sit = skipSeqs.find( sym );
        if( sit!=skipSeqs.end() )
            mfst.AddArc( 0, StdArc( i, 0, 0, 0 ) );
        else
            mfst.AddArc( 0, StdArc( i, i, 0, 0 ) );
    }
    mfst.SetFinal(0, 0);
    ILabelCompare<StdArc> icomp;
    ArcSort( &mfst, icomp );
    mfst.SetInputSymbols( osyms );
    mfst.SetOutputSymbols( osyms );
    
    return mfst;
}

StdVectorFst Phonetisaurus::entryToFSA( vector<string> entry ){
    /*
     Transform an input spelling/pronunciation into an equivalent
     FSA, adding extra arcs as needed to accomodate clusters.
    */
    
    StdVectorFst efst;
    efst.AddState();
    efst.SetStart(0);

    efst.AddState();
    efst.AddArc( 0, StdArc( isyms->Find( sb ), isyms->Find( sb ), 0, 1 ));
    size_t i=0;
    
    //Build the basic FSA
    for( i=0; i<entry.size(); i++){
        efst.AddState();
        string ch = entry[i];
        efst.AddArc( i+1, StdArc( isyms->Find(ch), isyms->Find(ch), 0, i+2 ));
        if( i==0 ) 
            continue;
        
    }
    
    //Add any cluster arcs
    map<vector<string>,int>::iterator it_i;
    for( it_i=clusters.begin(); it_i!=clusters.end(); it_i++ ){
        vector<string>::iterator it_j;
        vector<string>::iterator start = entry.begin();
        vector<string> cluster = (*it_i).first;
        while( it_j != entry.end() ){
            it_j = search( start, entry.end(), cluster.begin(), cluster.end() );
            if( it_j != entry.end() ){
                efst.AddArc( it_j-entry.begin()+1, StdArc( 
                        (*it_i).second,                     //input symbol
                        (*it_i).second,                     //output symbol
                        0,                                  //weight
                        it_j-entry.begin()+cluster.size()+1 //destination state
                    ) );
                start = it_j+cluster.size();
            }
        }
    }
    
    efst.AddState();
    efst.AddArc( i+1, StdArc( isyms->Find( se ), isyms->Find( se ), 0, i+2));
    efst.SetFinal( i+2, 0 );
    efst.SetInputSymbols( isyms );
    efst.SetOutputSymbols( isyms );
    efst.Write("efst");
    return efst;
}


vector<PathData> Phonetisaurus::phoneticize( vector<string> entry, int nbest ){
    /*
     Generate pronunciation/spelling hypotheses for an 
     input entry.
    */
    
    StdVectorFst result;
    StdVectorFst epsMapped;
    StdVectorFst shortest;

    StdVectorFst efst = entryToFSA( entry );

    Compose( efst, *g2pmodel, &result );

    Project(&result, PROJECT_OUTPUT);
    
    if( nbest > 1 ){
        //This is a cheesy hack. 
        ShortestPath( result, &shortest, 500 );
    }else{
        ShortestPath( result, &shortest, 1 );
    }
    
    RmEpsilon( &shortest );
    FstPathFinder pathfinder( skipSeqs );
    pathfinder.findAllStrings( shortest );
    
    return pathfinder.paths;
}


void Phonetisaurus::printPaths( vector<PathData> paths, int nbest, string correct ){
    /*
     Convenience function to print out a path vector.
     Will print only the first N unique entries.
    */

    set<string> seen;
    set<string>::iterator sit;
    
    int numseen = 0;
    string onepath;
    size_t k;
    for( k=0; k < paths.size(); k++ ){
        if ( k >= nbest )
            break;
        
        size_t j;
        for( j=0; j < paths[k].path.size(); j++ ){
            if( paths[k].path[j] != tie )
                replace( 
                        paths[k].path[j].begin(), 
                        paths[k].path[j].end(), 
                        '|', 
                        ' '
                        );
            onepath += paths[k].path[j];
            
            if( j != paths[k].path.size()-1 )
                onepath += " ";
        }

        cout << paths[k].pathcost << "\t" << onepath;
        if( correct != "" )
            cout << "\t" << correct;
        cout << endl;
        onepath = "";
    }
}









