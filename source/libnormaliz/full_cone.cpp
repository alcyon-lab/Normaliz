/*
 * Normaliz
 * Copyright (C) 2007-2013  Winfried Bruns, Bogdan Ichim, Christof Soeger
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

//---------------------------------------------------------------------------

#include <stdlib.h>
#include <set>
#include <map>
#include <iostream>
#include <string>
#include <algorithm>
#include <time.h>
#include <deque>

#include "full_cone.h"
#include "vector_operations.h"
#include "lineare_transformation.h"
#include "list_operations.h"
#include "map_operations.h"
#include "my_omp.h"
#include "integer.h"
// #include "sublattice_representation.h"

//---------------------------------------------------------------------------

const size_t RecBoundTriang=1000000;   //  if number(supphyps)*size(triang) > RecBoundTriang
                                       // we pass to (non-recirsive) pyramids

const size_t EvalBoundTriang=2500000; // if more than EvalBoundTriang simplices have been stored
                               // evaluation is started (whenever possible)

const size_t EvalBoundPyr=200000;   // the same for stored pyramids

const size_t EvalBoundRecPyr=20000;   // the same for stored RECURSIVE pyramids

const size_t IntermedRedBoundHB=2000000;  // bound for number of HB elements before 
                                              // intermediate reduction is called
                                              
const int largePyramidFactor=20;  // pyramid is large if largePyramidFactor*Comparisons[Pyramid_key.size()-dim] > old_nr_supp_hyps

const int SuppHypRecursionFactor=100; // pyramids for supphyps formed if Pos*Neg > this factor*dim^4


//---------------------------------------------------------------------------

namespace libnormaliz {
using namespace std;

//---------------------------------------------------------------------------
//private
//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::number_hyperplane(FACETDATA& hyp, const size_t born_at, const size_t mother){
// add identifying number, the birth day and the number of mother 

    hyp.Mother=mother;
    hyp.BornAt=born_at;
    if(!multithreaded_pyramid){
        hyp.Ident=HypCounter[0];
        HypCounter[0]++;
        return;
    }
    
    int tn;
    if(omp_get_level()==0)
        tn=0;
    else    
        tn = omp_get_ancestor_thread_num(1);
    hyp.Ident=HypCounter[tn];
    HypCounter[tn]+=omp_get_max_threads();
    
}

//---------------------------------------------------------------------------

template<typename Integer>
bool Full_Cone<Integer>::is_hyperplane_included(FACETDATA& hyp) {
    if (!is_pyramid) { // in the topcone we always have ov_sp > 0
        return true;
    }
    //check if it would be an excluded hyperplane
    Integer ov_sp = v_scalar_product(hyp.Hyp,Order_Vector);
    if (ov_sp > 0) {
        return true;
    } else if (ov_sp == 0) {
        for (size_t i=0; i<dim; i++) {
            if (hyp.Hyp[i]>0) {
                return true;
            } else if (hyp.Hyp[i]<0) {
                return false;
            }
        }
    }
    return false;
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::add_hyperplane(const size_t& new_generator, const FACETDATA & positive,const FACETDATA & negative,
                            list<FACETDATA>& NewHyps){
// adds a new hyperplane found in find_new_facets to this cone (restricted to generators processed)

    size_t k;
    
    FACETDATA NewFacet; NewFacet.Hyp.resize(dim); NewFacet.GenInHyp.resize(nr_gen);
    
    Integer used_for_tests;
    if (test_arithmetic_overflow) {  // does arithmetic tests
        for (k = 0; k <dim; k++) {
            NewFacet.Hyp[k]=positive.ValNewGen*negative.Hyp[k]-negative.ValNewGen*positive.Hyp[k];
            used_for_tests =(positive.ValNewGen%overflow_test_modulus)*(negative.Hyp[k]%overflow_test_modulus)-(negative.ValNewGen%overflow_test_modulus)*(positive.Hyp[k]%overflow_test_modulus);
            if (((NewFacet.Hyp[k]-used_for_tests) % overflow_test_modulus)!=0) {
                errorOutput()<<"Arithmetic failure in Full_Cone::add_hyperplane. Possible arithmetic overflow.\n";
                throw ArithmeticException();
            }
        }
    }
    else  {                      // no arithmetic tests
        for (k = 0; k <dim; k++) {
            NewFacet.Hyp[k]=positive.ValNewGen*negative.Hyp[k]-negative.ValNewGen*positive.Hyp[k];
        }
    }
    v_make_prime(NewFacet.Hyp);
    NewFacet.ValNewGen=0; 
    
    NewFacet.GenInHyp=positive.GenInHyp & negative.GenInHyp; // new hyperplane contains old gen iff both pos and neg do
    NewFacet.GenInHyp.set(new_generator);  // new hyperplane contains new generator
    number_hyperplane(NewFacet,nrGensInCone,positive.Ident);
    
    NewHyps.push_back(NewFacet);
}


//---------------------------------------------------------------------------


template<typename Integer>
void Full_Cone<Integer>::find_new_facets(const size_t& new_generator){
// our Fourier-Motzkin implementation
// the special treatment of simplicial facets was inserted because of line shellings.
// At present these are not computed.

    //to see if possible to replace the function .end with constant iterator since push-back is performed.

    // for dimension 0 and 1 F-M is never necessary and can lead to problems
    // when using dim-2
    if (dim <= 1)
        return;

    // NEW: new_generator is the index of the generator being inserted

    size_t i,k,nr_zero_i;
    size_t subfacet_dim=dim-2; // NEW dimension of subfacet
    size_t facet_dim=dim-1; // NEW dimension of facet
    
    const bool tv_verbose = false; //verbose && !is_pyramid; // && Support_Hyperplanes.size()>10000; //verbose in this method call
    
        
    // preparing the computations, the various types of facets are sorted into the deques
    deque <FACETDATA*> Pos_Simp,Pos_Non_Simp;
    deque <FACETDATA*> Neg_Simp,Neg_Non_Simp;
    deque <FACETDATA*> Neutral_Simp, Neutral_Non_Simp;
    
    boost::dynamic_bitset<> Zero_Positive(nr_gen),Zero_Negative(nr_gen);

    bool simplex;
    
    if (tv_verbose) verboseOutput()<<"transform_values:"<<flush;
    
    typename list<FACETDATA>::iterator ii = Facets.begin();
    
    for (; ii != Facets.end(); ++ii) {
        simplex=true;
        nr_zero_i=0;
        for (size_t j=0; j<nr_gen; j++){
            if (ii->GenInHyp.test(j)) {
                if (++nr_zero_i > facet_dim) {
                    simplex=false;
                    break;
                }
            }
        }
        
        if (ii->ValNewGen==0) {
            ii->GenInHyp.set(new_generator);  // Must be set explicitly !!
            if (simplex) {
                Neutral_Simp.push_back(&(*ii));
            }   else {
                Neutral_Non_Simp.push_back(&(*ii));
            }
        }
        else if (ii->ValNewGen>0) {
            Zero_Positive |= ii->GenInHyp;
            if (simplex) {
                Pos_Simp.push_back(&(*ii));
            } else {
                Pos_Non_Simp.push_back(&(*ii));
            }
        } 
        else if (ii->ValNewGen<0) {
            Zero_Negative |= ii->GenInHyp;
            if (simplex) {
                Neg_Simp.push_back(&(*ii));
            } else {
                Neg_Non_Simp.push_back(&(*ii));
            }
        }
    }
    
    // TO DO: Negativliste mit Zero_Positive verfeinern, also die aussondern, die nicht genug positive Erz enthalten
    // Eventuell sogar Rang-Test einbauen.
    // Letzteres k�nnte man auch bei den positiven machen, bevor sie verearbeitet werden
    
    boost::dynamic_bitset<> Zero_PN(nr_gen);
    Zero_PN = Zero_Positive & Zero_Negative;
    
    size_t nr_PosSimp  = Pos_Simp.size();
    size_t nr_PosNonSimp = Pos_Non_Simp.size();
    size_t nr_NegSimp  = Neg_Simp.size();
    size_t nr_NegNonSimp = Neg_Non_Simp.size();
    size_t nr_NeuSimp  = Neutral_Simp.size();
    size_t nr_NeuNonSimp = Neutral_Non_Simp.size();
    
    if (tv_verbose) verboseOutput()<<" PS "<<nr_PosSimp<<", P "<<nr_PosNonSimp<<", NS "<<nr_NegSimp<<", N "<<nr_NegNonSimp<<", ZS "<<nr_NeuSimp<<", Z "<<nr_NeuNonSimp<<endl;

    if (tv_verbose) verboseOutput()<<"transform_values: subfacet of NS: "<<flush;
    
    vector< list<pair < boost::dynamic_bitset<>, int> > > Neg_Subfacet_Multi(omp_get_max_threads()) ;

    boost::dynamic_bitset<> zero_i, subfacet;

    #pragma omp parallel for private(zero_i,subfacet,k,nr_zero_i)
    for (i=0; i<nr_NegSimp;i++){
        zero_i=Zero_PN & Neg_Simp[i]->GenInHyp;
        
        nr_zero_i=0;
        for(size_t j=0;j<nr_gen;j++){
            if(zero_i.test(j))
                nr_zero_i++;
            if(nr_zero_i>subfacet_dim){
                break;
            }
        }

        if(nr_zero_i==subfacet_dim) // NEW This case treated separately
            Neg_Subfacet_Multi[omp_get_thread_num()].push_back(pair <boost::dynamic_bitset<>, int> (zero_i,i));
            
        else {
            for (k =0; k<nr_gen; k++) {  
                if(zero_i.test(k)) {              
                    subfacet=zero_i;
                    subfacet.reset(k);  // remove k-th element from facet to obtain subfacet
                    Neg_Subfacet_Multi[omp_get_thread_num()].push_back(pair <boost::dynamic_bitset<>, int> (subfacet,i));
                }
            }
        }
    }
    
    list < pair < boost::dynamic_bitset<>, int> > Neg_Subfacet_Multi_United;
    for(int i=0;i<omp_get_max_threads();++i)
        Neg_Subfacet_Multi_United.splice(Neg_Subfacet_Multi_United.begin(),Neg_Subfacet_Multi[i]);
    Neg_Subfacet_Multi_United.sort();


    if (tv_verbose) verboseOutput()<<Neg_Subfacet_Multi_United.size() << ", " << flush;

    list< pair < boost::dynamic_bitset<>, int > >::iterator jj;
    list< pair < boost::dynamic_bitset<>, int > >::iterator del;
    jj =Neg_Subfacet_Multi_United.begin();           // remove negative subfacets shared
    while (jj!= Neg_Subfacet_Multi_United.end()) {   // by two neg simpl facets
        del=jj++;
        if (jj!=Neg_Subfacet_Multi_United.end() && (*jj).first==(*del).first) {   //delete since is the intersection of two negative simplicies
            Neg_Subfacet_Multi_United.erase(del);
            del=jj++;
            Neg_Subfacet_Multi_United.erase(del);
        }
    }

    size_t nr_NegSubfMult = Neg_Subfacet_Multi_United.size();
    if (tv_verbose) verboseOutput() << nr_NegSubfMult << ", " << flush;
    
    vector<list<FACETDATA> > NewHypsSimp(nr_PosSimp);
    vector<list<FACETDATA> > NewHypsNonSimp(nr_PosNonSimp);

    map < boost::dynamic_bitset<>, int > Neg_Subfacet;
    size_t nr_NegSubf=0;
    
    // size_t NrMatches=0, NrCSF=0, NrRank=0, NrComp=0, NrNewF=0;
    
    /* deque<bool> Indi(nr_NegNonSimp);
    for(size_t j=0;j<nr_NegNonSimp;++j)
        Indi[j]=false; */
        
    if(multithreaded_pyramid){
        #pragma omp atomic
        nrTotalComparisons+=nr_NegNonSimp*nr_PosNonSimp;
    }
    else{
        nrTotalComparisons+=nr_NegNonSimp*nr_PosNonSimp; 
    } 

    
//=====================================================================
// parallel from here

    #pragma omp parallel private(jj) if(nr_NegNonSimp+nr_NegSimp>1000)
    {
    size_t i,j,k,nr_zero_i;
    boost::dynamic_bitset<> subfacet(dim-2);
    jj = Neg_Subfacet_Multi_United.begin();
    size_t jjpos=0;

    bool found;
    #pragma omp for schedule(dynamic)
    for (size_t j=0; j<nr_NegSubfMult; ++j) {  // remove negative subfacets shared
        for(;j > jjpos; ++jjpos, ++jj) ;       // by non-simpl neg or neutral facets 
        for(;j < jjpos; --jjpos, --jj) ;

        subfacet=(*jj).first;
        found=false; 
        for (i = 0; i <nr_NeuSimp; i++) {
            found=subfacet.is_subset_of(Neutral_Simp[i]->GenInHyp);
            if(found)
                break;
        }
        if (!found) {
            for (i = 0; i <nr_NeuNonSimp; i++) {
                found=subfacet.is_subset_of(Neutral_Non_Simp[i]->GenInHyp);
                if(found)
                    break;                    
            }
            if(!found) {
                for (i = 0; i <nr_NegNonSimp; i++) {
                    found=subfacet.is_subset_of(Neg_Non_Simp[i]->GenInHyp);
                    if(found)
                        break; 
                }
            }
        }
        if (found) {
            jj->second=-1;
        }
    }
    
    #pragma omp single
    { //remove elements that where found in the previous loop
    jj = Neg_Subfacet_Multi_United.begin();
    map < boost::dynamic_bitset<>, int > ::iterator last_inserted=Neg_Subfacet.begin(); // used to speedup insertion into the new map
    for (; jj!= Neg_Subfacet_Multi_United.end(); ++jj) {
        if ((*jj).second != -1) {
            last_inserted = Neg_Subfacet.insert(last_inserted,*jj);
        }
    }
    nr_NegSubf=Neg_Subfacet.size();
    }
    
    #pragma omp single nowait
    {Neg_Subfacet_Multi_United.clear();}

    
    boost::dynamic_bitset<> zero_i(nr_gen);
    map <boost::dynamic_bitset<>, int> ::iterator jj_map;

    
    #pragma omp single nowait
    if (tv_verbose) {
        verboseOutput() << "PS vs NS and PS vs N , " << flush;
    }

    vector<key_t> key(nr_gen);
    size_t nr_missing;
    bool common_subfacet;
    #pragma omp for schedule(dynamic) nowait
    for (size_t i =0; i<nr_PosSimp; i++){ //Positive Simp vs.Negative Non Simp

        zero_i=Zero_PN & Pos_Simp[i]->GenInHyp;
        nr_zero_i=0;
        for(j=0;j<nr_gen && nr_zero_i<=facet_dim;j++)
            if(zero_i.test(j)){
                key[nr_zero_i]=j;
                nr_zero_i++;
            } 
            
        if(nr_zero_i<subfacet_dim)
            continue;
            
        // first PS vs NS
        
        if (nr_zero_i==subfacet_dim) {                 // NEW slight change in logic. Positive simpl facet shared at most
            jj_map=Neg_Subfacet.find(zero_i);           // one subfacet with negative simpl facet
            if (jj_map!=Neg_Subfacet.end()) {
                add_hyperplane(new_generator,*Pos_Simp[i],*Neg_Simp[(*jj_map).second],NewHypsSimp[i]);
                (*jj_map).second = -1;  // block subfacet in further searches
            }
        }
        if (nr_zero_i==facet_dim){    // now there could be more such subfacets. We make all and search them.      
            for (k =0; k<nr_gen; k++) {  // BOOST ROUTINE
                if(zero_i.test(k)) { 
                    subfacet=zero_i;
                    subfacet.reset(k);  // remove k-th element from facet to obtain subfacet
                    jj_map=Neg_Subfacet.find(subfacet);
                    if (jj_map!=Neg_Subfacet.end()) {
                        add_hyperplane(new_generator,*Pos_Simp[i],*Neg_Simp[(*jj_map).second],NewHypsSimp[i]);
                        (*jj_map).second = -1;
                        // Indi[j]=true;
                    }
                }
            }
        }

        // now PS vs N

       for (j=0; j<nr_NegNonSimp; j++){ // search negative facet with common subfacet
           nr_missing=0; 
           common_subfacet=true;               
           for(k=0;k<nr_zero_i;k++) {
               if(!Neg_Non_Simp[j]->GenInHyp.test(key[k])) {
                   nr_missing++;
                   if(nr_missing==2 || nr_zero_i==subfacet_dim) {
                       common_subfacet=false;
                       break;
                   }
               }
            }
               
            if(common_subfacet){                 
               add_hyperplane(new_generator,*Pos_Simp[i],*Neg_Non_Simp[j],NewHypsSimp[i]);
               if(nr_zero_i==subfacet_dim) // only one subfacet can lie in negative hyperplane
                   break;
            }
       }           
    } // PS vs NS and PS vs N

    #pragma omp single nowait
    if (tv_verbose) {
        verboseOutput() << "P vs NS and P vs N" << endl;
    }

    list<FACETDATA*> AllNonSimpHyp;
    typename list<FACETDATA*>::iterator a;

    for(i=0;i<nr_PosNonSimp;++i)
        AllNonSimpHyp.push_back(&(*Pos_Non_Simp[i]));
    for(i=0;i<nr_NegNonSimp;++i)
        AllNonSimpHyp.push_back(&(*Neg_Non_Simp[i]));
    for(i=0;i<nr_NeuNonSimp;++i)
        AllNonSimpHyp.push_back(&(*Neutral_Non_Simp[i])); 
    size_t nr_NonSimp = nr_PosNonSimp+nr_NegNonSimp+nr_NeuNonSimp;
   
    bool ranktest;
    FACETDATA *hp_i, *hp_j, *hp_t; // pointers to current hyperplanes
    
    size_t missing_bound, nr_common_zero;
    boost::dynamic_bitset<> common_zero(nr_gen);
    vector<key_t> common_key(nr_gen);
    vector<int> key_start(nrGensInCone);
    
    #pragma omp for schedule(dynamic) // nowait
    for (size_t i =0; i<nr_PosNonSimp; i++){ //Positive Non Simp vs.Negative Simp and Non Simp

        jj_map = Neg_Subfacet.begin();       // First the Simp
        for (j=0; j<nr_NegSubf; ++j,++jj_map) {
            if ( (*jj_map).second != -1 ) {  // skip used subfacets
                if(jj_map->first.is_subset_of(Pos_Non_Simp[i]->GenInHyp)){
                    add_hyperplane(new_generator,*Pos_Non_Simp[i],*Neg_Simp[(*jj_map).second],NewHypsNonSimp[i]);
                    (*jj_map).second = -1; // has now been used
                }
            }
        }
        
        // Now the NonSimp

        hp_i=Pos_Non_Simp[i];
        zero_i=Zero_PN & hp_i->GenInHyp;
        nr_zero_i=0;
        int last_existing=-1;
        for(size_t jj=0;jj<nrGensInCone;jj++)
        {
            j=GensInCone[jj];
            if(zero_i.test(j)){
                key[nr_zero_i]=j;
                for(size_t kk= last_existing+1;kk<=jj;kk++)
                    key_start[kk]=nr_zero_i;
                nr_zero_i++;
                last_existing= jj;
            }
        }
        if(last_existing< (int)nrGensInCone-1)
            for(size_t kk=last_existing+1;kk<nrGensInCone;kk++)
                key_start[kk]=nr_zero_i;
                
        if (nr_zero_i<subfacet_dim) 
            continue;
        
       missing_bound=nr_zero_i-subfacet_dim; // at most this number of generators can be missing
                                             // to have a chance for common subfacet                                            
       
       for (j=0; j<nr_NegNonSimp; j++){
    
        
           hp_j=Neg_Non_Simp[j];
           
           if(hp_i->Ident==hp_j->Mother || hp_j->Ident==hp_i->Mother){   // mother and daughter coming together
               add_hyperplane(new_generator,*hp_i,*hp_j,NewHypsNonSimp[i]);  // their intersection is a subfacet
               continue;           
           } 
           
           
           bool extension_test=hp_i->BornAt==hp_j->BornAt || (hp_i->BornAt<hp_j->BornAt && hp_j->Mother!=0)
                                                          || (hp_j->BornAt<hp_i->BornAt && hp_i->Mother!=0);
                                                          
           // extension_test=false;
                                                          
           size_t both_existing_from=key_start[max(hp_i->BornAt,hp_j->BornAt)];
                      
           nr_missing=0; 
           nr_common_zero=0;
           size_t second_loop_bound=nr_zero_i;
           common_subfacet=true;  
           
           if(extension_test){
               bool extended=false;
               second_loop_bound=both_existing_from;
               for(k=both_existing_from;k<nr_zero_i;k++){
                   if(!hp_j->GenInHyp.test(key[k])) {
                       nr_missing++;
                       if(nr_missing>missing_bound) {
                           common_subfacet=false;
                           break;
                       }
                   }
                   else {
                       extended=true;
                       common_key[nr_common_zero]=key[k];
                       nr_common_zero++;
                   }
               }

               if(!extended || !common_subfacet) // 
                   continue;
           }
                    
           
           for(k=0;k<second_loop_bound;k++) {
               if(!hp_j->GenInHyp.test(key[k])) {
                   nr_missing++;
                   if(nr_missing>missing_bound) {
                       common_subfacet=false;
                       break;
                   }
               }
               else {
                   common_key[nr_common_zero]=key[k];
                   nr_common_zero++;
               }
            }
            
           if(!common_subfacet)
                continue;
           /* #pragma omp atomic
           NrCSF++;*/
           
           ranktest = (nr_NonSimp > dim*dim*nr_common_zero/3);
           // ranktest=true;

           if(ranktest) {
           
           /* #pragma omp atomic
            NrRank++; */
            
               Matrix<Integer> Test(nr_common_zero,dim);
               for (k = 0; k < nr_common_zero; k++)
                   Test.write(k,Generators[common_key[k]]);

               if (Test.rank_destructive()<subfacet_dim) {
                   common_subfacet=false;
               }
           } // ranktest
           else{                 // now the comparison test
           
           /* #pragma omp atomic
            NrComp++; */
            
               common_zero = zero_i & hp_j->GenInHyp;
               for (a=AllNonSimpHyp.begin();a!=AllNonSimpHyp.end();++a){
                   hp_t=*a;
                   if ((hp_t!=hp_i) && (hp_t!=hp_j) && common_zero.is_subset_of(hp_t->GenInHyp)) {                                
                       common_subfacet=false;
                       AllNonSimpHyp.splice(AllNonSimpHyp.begin(),AllNonSimpHyp,a);
                       break;
                   }
               }                       
           } // else
           if (common_subfacet) {  //intersection of i and j is a subfacet
               add_hyperplane(new_generator,*hp_i,*hp_j,NewHypsNonSimp[i]);
               /* #pragma omp atomic
                NrNewF++; */
                // Indi[j]=true;
           }
       }

    }
    } //END parallel
    
//=====================================================================
// parallel until here


    /* if(!is_pyramid)
      cout << "Matches " << NrMatches << " pot. common subf " << NrCSF << " rank test " <<  NrRank << " comp test "
        << NrComp << " neww hyps " << NrNewF << endl; */


    for(i=0;i<nr_PosSimp;i++)
        Facets.splice(Facets.end(),NewHypsSimp[i]);

    for(i=0;i<nr_PosNonSimp;i++)
        Facets.splice(Facets.end(),NewHypsNonSimp[i]);

    //removing the negative hyperplanes
    // now done in build_cone

    if (tv_verbose) verboseOutput()<<"transform_values: done"<<endl;
    
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::extend_triangulation(const size_t& new_generator){
// extends the triangulation of this cone by including new_generator
// simplicial facets save us from searching the "brother" in the existing triangulation
// to which the new simplex gets attached

    size_t listsize =old_nr_supp_hyps; // Facets.size();
    vector<typename list<FACETDATA>::iterator> visible;
    visible.reserve(listsize);
    typename list<FACETDATA>::iterator i = Facets.begin();

    // #pragma omp critical(VERBOSE)
    // verboseOutput() << "L " << pyr_level << " H " << listsize << " T " << TriangulationSize << endl;
    
    listsize=0;
    for (; i!=Facets.end(); ++i) 
        if (i->ValNewGen < 0){ // visible facet
            visible.push_back(i);
            listsize++;
        }

    // listsize = visible.size(); // now acczmulated above
    // cout << "Pyr Level " << pyr_level << " Visible " << listsize <<  " Triang " << TriangulationSize << endl;


    typename list< SHORTSIMPLEX<Integer> >::iterator oldTriBack = --Triangulation.end();
    #pragma omp parallel private(i)  if(TriangulationSize>100)
    {
    size_t k,l;
    bool one_not_in_i, not_in_facet;
    size_t not_in_i=0;
    size_t facet_dim=dim-1;
    size_t nr_in_i=0;

    list< SHORTSIMPLEX<Integer> > Triangulation_kk;
    typename list< SHORTSIMPLEX<Integer> >::iterator j;
    
    vector<key_t> key(dim);
    
    // if we only want a partial triangulation but came here because of a deep level
    // mark if this part of the triangulation has not to be evaluated
    bool skip_eval = false;

    #pragma omp for schedule(dynamic)
    for (size_t kk=0; kk<listsize; ++kk) {
        i=visible[kk];
        
        nr_in_i=0;
        for(size_t m=0;m<nr_gen;m++){
            if(i->GenInHyp.test(m))
                nr_in_i++;
            if(nr_in_i>facet_dim){
                break;
            }
        }
        skip_eval = Top_Cone->do_partial_triangulation && i->ValNewGen == -1
                    && is_hyperplane_included(*i);

        if (nr_in_i==facet_dim){  // simplicial
            l=0;
            for (k = 0; k <nr_gen; k++) {
                if (i->GenInHyp[k]==1) {
                    key[l]=k;
                    l++;
                }
            }
            key[dim-1]=new_generator;
 
           if (skip_eval)
                store_key(key,0,0,Triangulation_kk);
            else
                store_key(key,-i->ValNewGen,0,Triangulation_kk);
            continue;
        } // end simplicial
        
        size_t irrelevant_vertices=0;
        for(size_t vertex=0;vertex<nrGensInCone;++vertex){
        
            if(i->GenInHyp[GensInCone[vertex]]==0) // lead vertex not in hyperplane
                continue;
                
            if(irrelevant_vertices<dim-2){
                ++irrelevant_vertices;
                continue;
            }       
        
            j=TriSectionFirst[vertex];
            bool done=false;
            for(;!done;j++)
            {
              done=(j==TriSectionLast[vertex]);
              key=j->key;
              one_not_in_i=false;  // true indicates that one gen of simplex is not in hyperplane
              not_in_facet=false;  // true indicates that a second gen of simplex is not in hyperplane
              for(k=0;k<dim;k++){
                 if ( !i->GenInHyp.test(key[k])) {
                     if(one_not_in_i){
                         not_in_facet=true;
                         break;
                     }
                     one_not_in_i=true;
                     not_in_i=k;
                  }
              }
              
              if(not_in_facet) // simplex does not share facet with hyperplane
                 continue;
              
              key[not_in_i]=new_generator;              
              if (skip_eval)
                  store_key(key,0,j->vol,Triangulation_kk);
              else
                  store_key(key,-i->ValNewGen,j->vol,Triangulation_kk);
                       
            } // j
            
        } // for vertex

    } // for kk

    if (multithreaded_pyramid) {
        #pragma omp critical(TRIANG)
        Triangulation.splice(Triangulation.end(),Triangulation_kk);
    } else
        Triangulation.splice(Triangulation.end(),Triangulation_kk);

    } // parallel

    // GensInCone.push_back(new_generator); // now in extend_cone
    TriSectionFirst.push_back(++oldTriBack);
    TriSectionLast.push_back(--Triangulation.end());    
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::store_key(const vector<key_t>& key, const Integer& height,
            const Integer& mother_vol, list< SHORTSIMPLEX<Integer> >& Triangulation){
// stores a simplex given by key and height in Triangulation
// mother_vol is the volume of the simplex to which the new one is attached

    SHORTSIMPLEX<Integer> newsimplex;
    newsimplex.key=key;
    newsimplex.height=height;
    newsimplex.vol=0;
    
    if(multithreaded_pyramid){
        #pragma omp atomic
        TriangulationSize++;
    }
    else {
        TriangulationSize++;    
    }
    int tn;
    if(omp_get_level()==0)
        tn=0;
    else    
        tn = omp_get_ancestor_thread_num(1);
    
    if (do_only_multiplicity) {
        // directly compute the volume
        if (mother_vol==1)
            newsimplex.vol = height;
        // the multiplicity is computed in SimplexEvaluator
        for(size_t i=0; i<dim; i++) // and needs the key in TopCone numbers
            newsimplex.key[i]=Top_Key[newsimplex.key[i]];

        if (keep_triangulation)
            sort(newsimplex.key.begin(),newsimplex.key.end());
        Top_Cone->SimplexEval[tn].evaluate(newsimplex);
        // restore the local generator numbering, needed in extend_triangulation
        newsimplex.key=key;
    }
    
    if (keep_triangulation){
        Triangulation.push_back(newsimplex);
        return;  
    }
    
    bool Simpl_available=true;

    typename list< SHORTSIMPLEX<Integer> >::iterator F;

    if(Top_Cone->FS[tn].empty()){
        #pragma omp critical(FREESIMPL)
        {
        if(Top_Cone->FreeSimpl.empty())
            Simpl_available=false;
        else{
            F=Top_Cone->FreeSimpl.begin();  // take 100 simplices from FreeSimpl
            size_t q; for(q=0;q<1000;++q){            // or what you can get
                if(F==Top_Cone->FreeSimpl.end())
                    break;
                ++F;
            }
        
            if(q<1000)
                Top_Cone->FS[tn].splice(Top_Cone->FS[tn].begin(),
                    Top_Cone->FreeSimpl);
            else
                Top_Cone->FS[tn].splice(Top_Cone->FS[tn].begin(),
                              Top_Cone->FreeSimpl,Top_Cone->FreeSimpl.begin(),F);
        } // else
        } // critical
    } // if empty
          

    if(Simpl_available){
        Triangulation.splice(Triangulation.end(),Top_Cone->FS[tn],
                        Top_Cone->FS[tn].begin());
        Triangulation.back()=newsimplex;
    }
    else
        Triangulation.push_back(newsimplex);
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::process_pyramids(const size_t new_generator,const bool recursive){

    /*

    We distinguish two types of pyramids:

    (i) recursive pyramids that give their support hyperplanes back to the mother.
    (ii) independent pyramids that are not linked to the mother.

    The parameter "recursive" indicates whether the pyramids that will be created
    in process_pyramid(s) are of type (i) or (ii).

    Every pyramid can create subpyramids of both types (not the case in version 2.8 - 2.10).

    Whether "this" is of type (i) or (ii) is indicated by do_all_hyperplanes.

    The creation of (sub)pyramids of type (i) can be blocked by setting recursion_allowed=false.
    (Not done in this version.)

    is_pyramid==false for the top_cone and ==true else.

    multithreaded_pyramid indicates whether parallelization takes place within the
    computation of a pyramid or whether it is computed in a single thread (defined in build_cone).

    Recursie pyramids are processed immediately after creation (as in 2.8). However, there
    are two exceptions:

    (a) In order to avoid very long waiting times for the computation of the "large" ones,
    these are treated as follows: the support hyperplanes of "this" coming from their bases
    (as negative hyperplanes of "this") are computed by matching them with the
    positive hyperplanes of "this". This Fourier-Motzkin step is much more
    efficient if a pyramid is large. For triangulation a large recursive
    pyramid is then stored as a pyramid of type (ii).

    (b) If the "this" is processed in a parallelized loop calling process_pyramids, then
    the loop in process_pyramids cannot be interrupted for the evaluation of simplices. As a
    consequence an extremely long lst of simplices could arise if many small subpyramids of "this"
    are created in process_pyramids. In order to prevent this dangeous effect, small recursive
    subpyramids are stored for later triangulation if the simplex buffer has reached its
    size bound.

    Pyramids of type (ii) are stpred in Pyramids. The store_level of the created pyramids is 0
    for all pyramids created (possibly recursively) from the top cone. Pyramids created
    in evaluate_stored_pyramids get the store level for their subpyramids in that routine.

    Note: the top cone has pyr_level=-1. The pyr_level has no algorithmic relevance
    at present, but it shows the depth of the pyramid recursion at which the pyramid has been
    created.

    */


    size_t start_level=omp_get_level(); // allows us to check that we are on level 0
                                        // outside the loop and can therefore call evaluation
                                        // in order to empty the buffers
    vector<key_t> Pyramid_key;
    Pyramid_key.reserve(nr_gen);
    bool skip_triang; // make hyperplanes but skip triangulation (recursive pyramids only)

    deque<bool> done(old_nr_supp_hyps,false);
    bool skip_remaining_tri,skip_remaining_pyr;
    typename list< FACETDATA >::iterator hyp;
    size_t nr_done=0;

    do{  // repeats processing until all hyperplanes have been processed

    hyp=Facets.begin();
    size_t hyppos=0;
    skip_remaining_tri=skip_remaining_pyr=false;


    #pragma omp parallel for private(skip_triang) firstprivate(hyppos,hyp,Pyramid_key) schedule(dynamic) reduction(+: nr_done)
    for (size_t kk=0; kk<old_nr_supp_hyps; ++kk) {

        if(skip_remaining_tri || skip_remaining_pyr)
            continue;

        for(;kk > hyppos; hyppos++, hyp++) ;
        for(;kk < hyppos; hyppos--, hyp--) ;

        if(done[hyppos])
            continue;

        done[hyppos]=true;
        nr_done++;

        if (hyp->ValNewGen == 0)                     // MUST BE SET HERE
            hyp->GenInHyp.set(new_generator);

        if (hyp->ValNewGen >= 0) // facet not visible
            continue;

        skip_triang = false;
        if (Top_Cone->do_partial_triangulation && hyp->ValNewGen>=-1) { //ht1 criterion
            skip_triang = is_hyperplane_included(*hyp);
            if (skip_triang && !recursive) {
                continue;
            }
        }

        Pyramid_key.clear(); // make data of new pyramid
        Pyramid_key.push_back(new_generator);
        for(size_t i=0;i<nr_gen;i++){
            if(in_triang[i] && hyp->GenInHyp.test(i)) {
                Pyramid_key.push_back(i);
            }
        }

        // now we can store the new pyramid at the right place (or finish the simplicial ones)
        if (recursive && skip_triang) { // mark as "do not triangulate"
            process_pyramid(Pyramid_key, new_generator,store_level,0, recursive,hyp,start_level);
        } else { //default
            process_pyramid(Pyramid_key, new_generator,store_level,-hyp->ValNewGen, recursive,hyp,start_level);
        }

        if (start_level==0) {  // interrupt parallel execution if it is really parallel
            if(check_evaluation_buffer_size())
                skip_remaining_tri=true;  // keep the triangulation buffer under control
            if(Top_Cone->nrPyramids[store_level] > EvalBoundPyr)
                skip_remaining_pyr=true;  // keep the pyramid buffer under control
        }
        
        
    } // end parallel loop over hyperplanes

    
    if (skip_remaining_tri) {
        Top_Cone->evaluate_triangulation();
    }
    
    if (skip_remaining_pyr) {
        Top_Cone->evaluate_stored_pyramids(store_level);
    }

    } while (nr_done < old_nr_supp_hyps);
    
    evaluate_large_rec_pyramids(new_generator);

}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::process_pyramid(const vector<key_t>& Pyramid_key,
                          const size_t new_generator,const size_t store_level, Integer height, const bool recursive,
                          typename list< FACETDATA >::iterator hyp, size_t start_level){
// processes simplicial pyramids directly, stores other pyramids into their depots

    #pragma omp atomic
    Top_Cone->totalNrPyr++;

    if(Pyramid_key.size()==dim){  // simplicial pyramid completely done here
        #pragma omp atomic        // only for saving memory
        Top_Cone->nrSimplicialPyr++;
        if(recursive){ // the facets may be facets of the mother cone and if recursive==true must be given back
            Simplex<Integer> S(Pyramid_key, Generators);
            if (height != 0)
                height = S.read_volume(); //update our lower bound for the volume
            Matrix<Integer> H=S.read_support_hyperplanes();
            list<FACETDATA> NewFacets;
            FACETDATA NewFacet;
            NewFacet.GenInHyp.resize(nr_gen);
            for (size_t i=0; i<dim;i++) {
                NewFacet.Hyp = H[i];
                NewFacet.GenInHyp.set();
                NewFacet.GenInHyp.reset(i);
                NewFacets.push_back(NewFacet);
            }
            select_supphyps_from(NewFacets,new_generator,Pyramid_key); // takes itself care of multithreaded_pyramid
        }
        if (height != 0 && (do_triangulation || do_partial_triangulation)) {
            if(multithreaded_pyramid) {
                #pragma omp critical(TRIANG)
                {
                store_key(Pyramid_key,height,0,Triangulation);
                nrTotalComparisons+=dim*dim/2;
                }
            } else {
                store_key(Pyramid_key,height,0,Triangulation);
                nrTotalComparisons+=dim*dim/2;
            }
        }
    }
    else {  // non-simplicial
    
        bool large=(largePyramidFactor*Comparisons[Pyramid_key.size()-dim] > old_nr_supp_hyps); // Pyramid_key.size()>largePyramidFactor*dim;
        
        if (!recursive || (large && (do_triangulation || do_partial_triangulation) && height!=0) ) {  // must also store for triangulation if recursive and large
            vector<key_t> key_wrt_top(Pyramid_key.size());
            for(size_t i=0;i<Pyramid_key.size();i++)
                key_wrt_top[i]=Top_Key[Pyramid_key[i]];
            #pragma omp critical(STOREPYRAMIDS)
            {
            //      cout << "store_level " << store_level << " large " << large << " pyr level " << pyr_level << endl;
            Top_Cone->Pyramids[store_level].push_back(key_wrt_top);
            Top_Cone->nrPyramids[store_level]++;
            } // critical
            if(!recursive)    // in this case we need only store for future triangulation, and that has been done
                return;
        }
        // now we are in the recursive case and must compute support hyperplanes of the subpyramid
        if(large){  // large recursive pyramid
            if(multithreaded_pyramid){
                #pragma omp critical(LARGERECPYRS)
                LargeRecPyrs.push_back(*hyp);  // LargeRecPyrs are kept and evaluated locally
            }
            else
                LargeRecPyrs.push_back(*hyp);
            return; // done with the large recusive pyramids
        }

        // only recursive small ones left

        Full_Cone<Integer> Pyramid(*this,Pyramid_key);
        Pyramid.Mother = this;
        Pyramid.Mother_Key = Pyramid_key;    // need these data to give back supphyps
        Pyramid.apex=new_generator;
        if (height == 0) { //indicates "do not triangulate"
            Pyramid.do_triangulation = false;
            Pyramid.do_partial_triangulation = false;
            Pyramid.do_Hilbert_basis = false;
            Pyramid.do_deg1_elements=false;
        }

        bool store_for_triangulation=(store_level!=0) //loop in process_pyramids cannot be interrupted
            && (Pyramid.do_triangulation || Pyramid.do_partial_triangulation) // we must (partially) triangulate
            && (start_level!=0 && Top_Cone->TriangulationSize > 2*EvalBoundTriang); // evaluation buffer already full  // EvalBoundTriang

        if (store_for_triangulation) {
            vector<key_t> key_wrt_top(Pyramid_key.size());
            for(size_t i=0;i<Pyramid_key.size();i++)
                key_wrt_top[i]=Top_Key[Pyramid_key[i]];
            #pragma omp critical(STOREPYRAMIDS)
            {
            Top_Cone->Pyramids[store_level].push_back(key_wrt_top);
            Top_Cone->nrPyramids[store_level]++;
            } // critical
            // Now we must suppress immediate triangulation
            Pyramid.do_triangulation = false;
            Pyramid.do_partial_triangulation = false;
            Pyramid.do_Hilbert_basis = false;
            Pyramid.do_deg1_elements=false;
        }

        Pyramid.build_cone();

        if(multithreaded_pyramid){
            #pragma omp atomic
            nrTotalComparisons+=Pyramid.nrTotalComparisons;
        } else
            nrTotalComparisons+=Pyramid.nrTotalComparisons;
    }  // else non-simplicial
}


//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::find_and_evaluate_start_simplex(){

    size_t i,j;
    Integer factor;

    
    Simplex<Integer> S = find_start_simplex();
    vector<key_t> key=S.read_key();   // generators indexed from 0
        
    for (i = 0; i < dim; i++) {
        in_triang[key[i]]=true;
        GensInCone.push_back(key[i]);
        if (deg1_triangulation && isComputed(ConeProperty::Grading))
            deg1_triangulation = (gen_degrees[i] == 1);
    }
    
    nrGensInCone=dim;
    
    nrTotalComparisons=dim*dim/2;
    Comparisons.push_back(nrTotalComparisons);
       
    Matrix<Integer> H=S.read_support_hyperplanes();
    for (i = 0; i <dim; i++) {
        FACETDATA NewFacet; NewFacet.GenInHyp.resize(nr_gen);
        NewFacet.Hyp=H.read(i);
        for(j=0;j < dim;j++)
            if(j!=i)
                NewFacet.GenInHyp.set(key[j]);
        NewFacet.ValNewGen=-1;         // must be taken negative since opposite facet
        number_hyperplane(NewFacet,0,0); // created with gen 0
        Facets.push_back(NewFacet);    // was visible before adding this vertex
    }
    
    if(!is_pyramid){
        //define Order_Vector, decides which facets of the simplices are excluded
        Order_Vector = vector<Integer>(dim,0);
        Matrix<Integer> G=S.read_generators();
        //srand(12345);
        for(i=0;i<dim;i++){
            factor=(unsigned long)(2*(rand()%(2*dim))+3);
            for(j=0;j<dim;j++)
                Order_Vector[j]+=factor*G[i][j];        
        }
    }

    //the volume is an upper bound for the height
    if(do_triangulation || (do_partial_triangulation && S.read_volume()>1))
    {
        store_key(key,S.read_volume(),1,Triangulation); 
        if(do_only_multiplicity) {
            #pragma omp atomic
            TotDet++;
        }
    }
    
    if(do_triangulation){ // we must prepare the sections of the triangulation
        for(i=0;i<dim;i++)
        {
            // GensInCone.push_back(key[i]); // now done in first loop since always needed
            TriSectionFirst.push_back(Triangulation.begin());
            TriSectionLast.push_back(Triangulation.begin());
        }
    }
    
}


//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::select_supphyps_from(const list<FACETDATA>& NewFacets, 
                    const size_t new_generator, const vector<key_t>& Pyramid_key){
// the mother cone (=this) selects supphyps from the list NewFacets supplied by the daughter
// the daughter provides the necessary information via the parameters

    size_t i;
    boost::dynamic_bitset<> in_Pyr(nr_gen);
    for (i=0; i<Pyramid_key.size(); i++) {
        in_Pyr.set(Pyramid_key[i]);
    }
    // the new generator is always the first in the pyramid
    assert(Pyramid_key[0] == new_generator);


    typename list<FACETDATA>::const_iterator pyr_hyp = NewFacets.begin();
    bool new_global_hyp;
    FACETDATA NewFacet;
    NewFacet.GenInHyp.resize(nr_gen);
    Integer test;
    for (; pyr_hyp!=NewFacets.end(); ++pyr_hyp) {
        if(!pyr_hyp->GenInHyp.test(0)) // new gen not in hyp
            continue;
        new_global_hyp=true;
        for (i=0; i<nr_gen; ++i){
            if(in_Pyr.test(i) || !in_triang[i])
                continue;
            test=v_scalar_product(Generators[i],pyr_hyp->Hyp);
            if(test<=0){
                new_global_hyp=false;
                break;
            }

        }
        if(new_global_hyp){
            NewFacet.Hyp=pyr_hyp->Hyp;
            NewFacet.GenInHyp.reset();
            for (i=0; i<Pyramid_key.size(); ++i) {
                if (pyr_hyp->GenInHyp.test(i) && in_triang[Pyramid_key[i]]) {
                    NewFacet.GenInHyp.set(Pyramid_key[i]);
                }
            }
            NewFacet.GenInHyp.set(new_generator);
            number_hyperplane(NewFacet,nrGensInCone,0); //mother unknown
            if(multithreaded_pyramid){
                #pragma omp critical(GIVEBACKHYPS) 
                Facets.push_back(NewFacet);
            } else {
                Facets.push_back(NewFacet);
            }
        }
    }
}

//---------------------------------------------------------------------------
template<typename Integer>
void Full_Cone<Integer>::match_neg_hyp_with_pos_hyps(const FACETDATA& hyp, size_t new_generator,list<FACETDATA*>& PosHyps, boost::dynamic_bitset<>& Zero_P){

    size_t missing_bound, nr_common_zero;
    boost::dynamic_bitset<> common_zero(nr_gen);
    vector<key_t> common_key(nr_gen);
    vector<key_t> key(nr_gen);
    bool common_subfacet;
    list<FACETDATA> NewHyp;
    size_t subfacet_dim=dim-2;
    size_t nr_missing;
    typename list<FACETDATA*>::iterator a;
    // FACETDATA *hp_t;
    list<FACETDATA> NewHyps;
    
    // cout << "Rein " << old_nr_supp_hyps << endl;


    boost::dynamic_bitset<> zero_hyp=hyp.GenInHyp & Zero_P;  // we intersect with the set of gens in positive hyps
    
    size_t nr_zero_hyp=0;
    vector<int> key_start(nrGensInCone);
    size_t j;
    int last_existing=-1;
    for(size_t jj=0;jj<nrGensInCone;jj++)
    {
        j=GensInCone[jj];
        if(zero_hyp.test(j)){
            key[nr_zero_hyp]=j;
            for(size_t kk= last_existing+1;kk<=jj;kk++)
                key_start[kk]=nr_zero_hyp;
            nr_zero_hyp++;
            last_existing= jj;
        }
    }
    if(last_existing< (int)nrGensInCone-1)
        for(size_t kk=last_existing+1;kk<nrGensInCone;kk++)
            key_start[kk]=nr_zero_hyp;
            
    if (nr_zero_hyp<dim-2) 
        return;
    
    missing_bound=nr_zero_hyp-subfacet_dim; // at most this number of generators can be missing
                                          // to have a chance for common subfacet
                                          
    typename list< FACETDATA*>::iterator hp_j_iterator=PosHyps.begin();
    
    FACETDATA* hp_j;

    for (;hp_j_iterator!=PosHyps.end();++hp_j_iterator){ //match hyp with the given Pos
        hp_j=*hp_j_iterator;


      if(hyp.Ident==hp_j->Mother || hp_j->Ident==hyp.Mother){   // mother and daughter coming together
           add_hyperplane(new_generator,*hp_j,hyp,NewHyps);  // their intersection is a subfacet
           continue;           
       }
       
       
       bool extension_test=hyp.BornAt==hp_j->BornAt || (hyp.BornAt<hp_j->BornAt && hp_j->Mother!=0)
                                                      || (hp_j->BornAt<hyp.BornAt && hyp.Mother!=0);
                                                      
       size_t both_existing_from=key_start[max(hyp.BornAt,hp_j->BornAt)];
                  
       nr_missing=0; 
       nr_common_zero=0;
       size_t second_loop_bound=nr_zero_hyp;
       common_subfacet=true;  
       
       if(extension_test){
           bool extended=false;
           second_loop_bound=both_existing_from;
           for(size_t k=both_existing_from;k<nr_zero_hyp;k++){
               if(!hp_j->GenInHyp.test(key[k])) {
                   nr_missing++;
                   if(nr_missing>missing_bound) {
                       common_subfacet=false;
                       break;
                   }
               }
               else {
                   extended=true;
                   common_key[nr_common_zero]=key[k];
                   nr_common_zero++;
               }
           }

           if(!extended || !common_subfacet) // 
               continue;
       }
                
       for(size_t k=0;k<second_loop_bound;k++) {
           if(!hp_j->GenInHyp.test(key[k])) {
               nr_missing++;
               if(nr_missing>missing_bound) {
                   common_subfacet=false;
                   break;
               }
           }
           else {
               common_key[nr_common_zero]=key[k];
               nr_common_zero++;
           }
        }
        
       if(!common_subfacet)
            continue;
            
        Matrix<Integer> Test(nr_common_zero,dim); // only rank test since we have many supphyps anyway
        for (size_t k = 0; k < nr_common_zero; k++)
            Test[k]=Generators[common_key[k]];

        if (Test.rank_destructive()<subfacet_dim) 
            common_subfacet=false;     // don't make a hyperplane

        
        if(common_subfacet)
            add_hyperplane(new_generator,*hp_j,hyp,NewHyps); // if !common_subfacet this is skipped by continue
    } // for

    if(multithreaded_pyramid)
        #pragma omp critical(GIVEBACKHYPS)
        Facets.splice(Facets.end(),NewHyps);
    else
        Facets.splice(Facets.end(),NewHyps);
        
       // cout << "Raus" << endl;

}

//---------------------------------------------------------------------------
template<typename Integer>
void Full_Cone<Integer>::collect_pos_supphyps(list<FACETDATA*>& PosHyps, boost::dynamic_bitset<>& Zero_P, size_t& nr_pos){
           
    // positive facets are collected in a list
    
    typename list<FACETDATA>::iterator ii = Facets.begin();
    nr_pos=0;
    
    for (size_t ij=0; ij< old_nr_supp_hyps; ++ij, ++ii)
        if (ii->ValNewGen>0) {
            Zero_P |= ii->GenInHyp;
            PosHyps.push_back(&(*ii));
            nr_pos++;
        }
}

//---------------------------------------------------------------------------
template<typename Integer>
void Full_Cone<Integer>::evaluate_large_rec_pyramids(size_t new_generator){
    
    size_t nrLargeRecPyrs=LargeRecPyrs.size();
    if(nrLargeRecPyrs==0)
        return;
        
    if(verbose && !is_pyramid)
        verboseOutput() << "Large " << nrLargeRecPyrs << endl;
    
    list<FACETDATA*> PosHyps;
    boost::dynamic_bitset<> Zero_P(nr_gen);
    size_t nr_pos;
    collect_pos_supphyps(PosHyps,Zero_P,nr_pos);
    
    nrTotalComparisons+=nr_pos*nrLargeRecPyrs;
    
    #pragma omp parallel
    {                            
    size_t ppos=0;
    typename list<FACETDATA>::iterator p=LargeRecPyrs.begin(); 
    
    #pragma omp for schedule(dynamic) 
    for(size_t i=0; i<nrLargeRecPyrs; i++){
    for(; i > ppos; ++ppos, ++p) ;
    for(; i < ppos; --ppos, --p) ;
        match_neg_hyp_with_pos_hyps(*p,new_generator,PosHyps,Zero_P);
    }
    } // parallel

    LargeRecPyrs.clear();
    
}



//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::evaluate_stored_pyramids(const size_t level){
// evaluates the stored non-recursive pyramids
//
// OBOLETE:
// In contrast to the the recusrive pyramids, extend_cone is called
// only once for every stored pyramid since we set recursion_allowed=false.

    assert(omp_get_level()==0);

    if(Pyramids[level].empty())
        return;
    Pyramids.resize(level+2); // provide space for a new generation
    nrPyramids.resize(level+2);
    nrPyramids[level+1]=0;

    size_t nr_done=0;
    size_t nr_pyramids=nrPyramids[level];
    vector<char> Done(nr_pyramids,0);
    if (verbose) {
        verboseOutput() << "**************************************************" << endl;

        for (size_t l=0; l<=level; ++l) {
            if (nrPyramids[l]>0) {
                verboseOutput() << "level " << l << " pyramids remaining: "
                                << nrPyramids[l] << endl;
            }
        }
        verboseOutput() << "**************************************************" << endl;
    }
    typename list<vector<key_t> >::iterator p;
    size_t ppos;
    bool skip_remaining_tri,skip_remaining_pyr;

    while (nr_pyramids > 0) {

       p = Pyramids[level].begin();
       ppos=0;
       skip_remaining_tri=false;
       skip_remaining_pyr=false;
    
       #pragma omp parallel for firstprivate(p,ppos) schedule(dynamic) 
       for(size_t i=0; i<nr_pyramids; i++){
       
           if(skip_remaining_tri || skip_remaining_pyr)
                continue;
                
           for(; i > ppos; ++ppos, ++p) ;
           for(; i < ppos; --ppos, --p) ;
           
           if(Done[i])
               continue;
           Done[i]=1;
           
           /* if(ReportPyr){
           #pragma omp critical(REPORTPYR)
           cout << "Pyr " << i << ": " << *p << endl << flush;
           
           } */
            
           
           #pragma omp atomic
           nr_done++;
           
           Full_Cone<Integer> Pyramid(*this,*p);
           // Pyramid.recursion_allowed=false;
           Pyramid.do_all_hyperplanes=false;
           if(level>=2 && do_partial_triangulation){ // limits the descent of do_partial_triangulation
               Pyramid.do_triangulation=true;
               Pyramid.do_partial_triangulation=false;
           }
           Pyramid.store_level=level+1;
           Pyramid.build_cone();
           if(check_evaluation_buffer_size() && nr_done < nr_pyramids)  // we interrupt parallel execution if it is really parallel
                skip_remaining_tri=true;                         //  to keep the triangulation buffer under control
                
            if(nrPyramids[level+1]>EvalBoundPyr && nr_done < nr_pyramids) 
                 skip_remaining_pyr=true;
        }
       
        // remove done pyramids
        p = Pyramids[level].begin();
        for(size_t i=0; i<nr_pyramids; i++){
            if (Done[i]) {
                p=Pyramids[level].erase(p);
                nrPyramids[level]--;
                Done[i]=0;
            } else {
                ++p;
            }
        }
        nr_done=0;
        nr_pyramids = nrPyramids[level];

        if (skip_remaining_tri) {
            if (verbose)
                verboseOutput() << nr_pyramids <<
                    " pyramids remaining on level " << level << ", ";
            Top_Cone->evaluate_triangulation();
        }

        if(skip_remaining_pyr){
            evaluate_stored_pyramids(level+1);
        }
    
    } //end while (nr_pyramid > 0)
     
    if (verbose) {
        verboseOutput() << "**************************************************" << endl;
        verboseOutput() << "all pyramids on level "<< level << " done!"<<endl;
        if (nrPyramids[level+1] == 0) {
            for (size_t l=0; l<=level; ++l) {
                if (nrPyramids[l]>0) {
                    verboseOutput() << "level " << l << " pyramids remaining: "
                                    << nrPyramids[l] << endl;
                }
            }
            verboseOutput() << "**************************************************" << endl;
        }
    }
    if(check_evaluation_buffer())
    {
        Top_Cone->evaluate_triangulation();
    }
     
    Pyramids[level].clear();
    nrPyramids[level]=0;
    evaluate_stored_pyramids(level+1);
}
    


//---------------------------------------------------------------------------

/* builds the cone successively by inserting generators */
template<typename Integer>
void Full_Cone<Integer>::build_cone() {
    if(dim>0){            //correction needed to include the 0 cone;
    
    // cout << "Pyr " << pyr_level << endl;

    long long RecBoundSuppHyp = dim*dim;
    RecBoundSuppHyp *= RecBoundSuppHyp*SuppHypRecursionFactor; //dim^4 * 3000
    
    tri_recursion=false; 
    
    multithreaded_pyramid=(omp_get_level()==0);
    
    if(multithreaded_pyramid){
        HypCounter.resize(omp_get_max_threads());
        for(size_t i=0;i<HypCounter.size();++i)
            HypCounter[i]=i+1;
    } else{
        HypCounter.resize(1);
        HypCounter[0]=1;    
    }

    find_and_evaluate_start_simplex();
    
    size_t last_to_be_inserted; // good to know in case of do_all_hyperplanes==false
    last_to_be_inserted=nr_gen-1;  // because we don't need to compute support hyperplanes in this case 
    for(int j=nr_gen-1;j>=0;--j){
        if(isComputed(ConeProperty::ExtremeRays)){
            if(!in_triang[j] && Extreme_Rays[j]){
                last_to_be_inserted=j;
                break;
            }
        }
        else
            if(!in_triang[j]){
                last_to_be_inserted=j;
                break;
            }
    } // last_to_be_inserted now determined
    
    bool is_new_generator;
    typename list< FACETDATA >::iterator l;


    for (size_t i=0;i<nr_gen;++i) {
    
        if(in_triang[i] || (isComputed(ConeProperty::ExtremeRays) && !Extreme_Rays[i]))
            continue;
            
        if(do_triangulation && TriangulationSize > 2*RecBoundTriang) // emermergency brake
            tri_recursion=true;               // to switch off production of simplices in favor
                                              // of non-recursive pyramids
        Integer scalar_product;                                              
        is_new_generator=false;
        l=Facets.begin();
        old_nr_supp_hyps=Facets.size(); // Facets will be xtended in the loop 

        long long nr_pos=0; long long nr_neg=0;
        vector<Integer> L;           
        
        size_t lpos=0;
        #pragma omp parallel for private(L,scalar_product) firstprivate(lpos,l) reduction(+: nr_pos, nr_neg)
        for (size_t k=0; k<old_nr_supp_hyps; k++) {
            for(;k > lpos; lpos++, l++) ;
            for(;k < lpos; lpos--, l--) ;

            L=Generators[i];
            scalar_product=v_scalar_product(L,(*l).Hyp);            
            l->ValNewGen=scalar_product;
            if (scalar_product<0) {
                is_new_generator=true;
                nr_neg++;
            }
            if (scalar_product>0) {
                nr_pos++;
            }
        }  //end parallel for
        
        if(!is_new_generator)
            continue;

        // the i-th generator is used in the triangulation
        in_triang[i]=true;
        if (deg1_triangulation && isComputed(ConeProperty::Grading))
            deg1_triangulation = (gen_degrees[i] == 1);
        
            
        // First we test whether to go to recursive pyramids because of too many supphyps
        if (recursion_allowed && nr_neg*nr_pos > RecBoundSuppHyp) {  // use pyramids because of supphyps
            if (do_triangulation)
                tri_recursion = true; // We can not go back to classical triangulation
            if(check_evaluation_buffer()){
                // cout << "Evaluation Build Mitte" << endl;
                Top_Cone->evaluate_triangulation();
            }

            process_pyramids(i,true); //recursive
            lastGen=i;
            nextGen=i+1; 
        }
        else{ // now we check whether to go to pyramids because of the size of triangulation
              // once we have done so, we must stay with it
            if( tri_recursion || (do_triangulation 
                && (nr_neg*TriangulationSize > RecBoundTriang 
                    || 3*omp_get_max_threads()*TriangulationSize>EvalBoundTriang ))){ // go to pyramids because of triangulation
                if(check_evaluation_buffer()){
                    Top_Cone->evaluate_triangulation();
                }
                tri_recursion=true;
                process_pyramids(i,false); //non-recursive
            }
            else{  // no pyramids necesary
                if(do_partial_triangulation)
                    process_pyramids(i,false); // non-recursive
                if(do_triangulation)
                    extend_triangulation(i);
            }

            if(do_all_hyperplanes || i!=last_to_be_inserted) 
                find_new_facets(i);
        }
        
        // removing the negative hyperplanes if necessary
        if(do_all_hyperplanes || i!=last_to_be_inserted){
            l=Facets.begin();
            for (size_t j=0; j<old_nr_supp_hyps;j++){
                if (l->ValNewGen<0) {
                    l=Facets.erase(l);
                }
                else
                    ++l;
            }
        }
        
        GensInCone.push_back(i);
        nrGensInCone++;
        
        Comparisons.push_back(nrTotalComparisons);
        
        if(verbose && !is_pyramid) {
            verboseOutput() << "gen="<< i+1 <<", ";
            if (do_all_hyperplanes || i!=last_to_be_inserted) {
                verboseOutput() << Facets.size()<<" hyp";
            } else {
                verboseOutput() << Support_Hyperplanes.size()<<" hyp";
            }
            if(nrPyramids[0]>0)
                verboseOutput() << ", " << nrPyramids[0] << " pyr"; 
            if(do_triangulation||do_partial_triangulation)
                verboseOutput() << ", " << TriangulationSize << " simpl";
            verboseOutput()<< endl;
        }
        
    }  // loop over i
    
    if (is_pyramid && do_all_hyperplanes)  // must give supphyps back to mother
        Mother->select_supphyps_from(Facets, apex, Mother_Key);
    
    // transfer Facets --> SupportHyperplanes
    if (do_all_hyperplanes) {
        typename list<FACETDATA>::const_iterator IHV=Facets.begin();
        for(;IHV!=Facets.end();IHV++)
            Support_Hyperplanes.push_back(IHV->Hyp);
    }  
    
    transfer_triangulation_to_top(); // transfer remaining simplices to top
    if(check_evaluation_buffer()){
        // cout << "Evaluating in build_cone at end, pyr level " << pyr_level << endl;
        // cout << "Evaluation Build Ende " << is_pyramid << endl;
        Top_Cone->evaluate_triangulation();
    }  

    } // end if (dim>0)
    
    Facets.clear();
    is_Computed.set(ConeProperty::SupportHyperplanes);
    

}

//---------------------------------------------------------------------------
/* builds the top cone successively by inserting generators, computes all essential data
except global reduction */
template<typename Integer>
void Full_Cone<Integer>::build_top_cone() {
    
    if(dim==0)
        return;
    if (verbose) {
        verboseOutput()<<endl<<"************************************************************"<<endl;
        verboseOutput()<<"starting primal algorithm ";
        if (do_partial_triangulation) verboseOutput()<<"with partial triangulation ";
        if (do_triangulation) {
            verboseOutput()<<"with full triangulation ";
        }
        if (!do_triangulation && !do_partial_triangulation) verboseOutput()<<"(only support hyperplanes) ";
        verboseOutput()<<"..."<<endl;
    }

    build_cone();
        
    evaluate_stored_pyramids(0);  // force evaluation of remaining pyramids                    
    
    if(!keep_triangulation) // force evaluation of remaining simplices
        evaluate_triangulation();          
        
    if(keep_triangulation)  // in this case triangulation now complete and stored
        is_Computed.set(ConeProperty::Triangulation);
        
    if (verbose) {
        verboseOutput() << "Total number of pyramids = "<< totalNrPyr << ", among them simplicial " << nrSimplicialPyr << endl;
        // cout << "Uni "<< Unimod << " Ht1NonUni " << Ht1NonUni << " NonDecided " << NonDecided << " TotNonDec " << NonDecidedHyp<< endl;
        if(do_only_multiplicity)
            verboseOutput() << "Determinantes computed = " << TotDet << endl;
    }   

}

//---------------------------------------------------------------------------

template<typename Integer>
bool Full_Cone<Integer>::check_evaluation_buffer(){

    return(omp_get_level()==0 && check_evaluation_buffer_size());
}

//---------------------------------------------------------------------------

template<typename Integer>
bool Full_Cone<Integer>::check_evaluation_buffer_size(){

    return(!Top_Cone->keep_triangulation && 
               Top_Cone->TriangulationSize > EvalBoundTriang);
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::transfer_triangulation_to_top(){  // NEW EVA

    size_t i;

    // cout << "Pyr level " << pyr_level << endl;
    
    if(!is_pyramid) {  // we are in top cone
        if(check_evaluation_buffer()){
            evaluate_triangulation();
        }
        return;      // no transfer necessary
    }

    // now we are in a pyramid

    // cout << "In pyramid " << endl;
    int tn = 0;
    if (omp_in_parallel())
        tn = omp_get_ancestor_thread_num(1);
  
    typename list< SHORTSIMPLEX<Integer> >::iterator pyr_simp=Triangulation.begin();
    while (pyr_simp!=Triangulation.end()) {
        if (pyr_simp->height == 0) { // it was marked to be skipped
//            pyr_simp = Triangulation.erase(pyr_simp); //TODO splice to FreeSimp?
            Top_Cone->FS[tn].splice(Top_Cone->FS[tn].end(), Triangulation, pyr_simp++);
            --TriangulationSize;
        } else {
            for (i=0; i<dim; i++)  // adjust key to topcone generators
                pyr_simp->key[i]=Top_Key[pyr_simp->key[i]];
            ++pyr_simp;
        }
    }

    // cout << "Keys transferred " << endl;
    #pragma omp critical(TRIANG)
    {
        Top_Cone->Triangulation.splice(Top_Cone->Triangulation.end(),Triangulation);
        Top_Cone->TriangulationSize+=TriangulationSize;
    }
    TriangulationSize  =  0;
  
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::evaluate_triangulation(){

    assert(omp_get_level()==0);
   
    if(TriangulationSize>0)
    {
    const long VERBOSE_STEPS = 50;
    long step_x_size = TriangulationSize-VERBOSE_STEPS;
    if (verbose) {
        verboseOutput() << "evaluating "<<TriangulationSize<<" simplices" <<endl;
        /* verboseOutput() << "---------+---------+---------+---------+---------+"
                        << " (one | per 2%)" << endl;*/
    }
    
    totalNrSimplices+=TriangulationSize;

    if(do_evaluation && !do_only_multiplicity) {
    #pragma omp parallel 
    {
        typename list< SHORTSIMPLEX<Integer> >::iterator s = Triangulation.begin();
        size_t spos=0;
        int tn = omp_get_thread_num();
        #pragma omp for schedule(dynamic) 
        for(size_t i=0; i<TriangulationSize; i++){
            for(; i > spos; ++spos, ++s) ;
            for(; i < spos; --spos, --s) ;

            if(keep_triangulation || do_Stanley_dec)
                sort(s->key.begin(),s->key.end());
            SimplexEval[tn].evaluate(*s);
            if (verbose) {
                #pragma omp critical(VERBOSE)
                while ((long)(i*VERBOSE_STEPS) >= step_x_size) {
                    step_x_size += TriangulationSize;
                    verboseOutput() << "|" <<flush;
                }
            }
        }
        SimplexEval[tn].transfer_candidates();
    } // end parallel
    if (verbose)
        verboseOutput()  << endl;
    } // do_evaluation

    if (verbose)
    {
        verboseOutput() << totalNrSimplices << " simplices";
        if(do_Hilbert_basis)
            verboseOutput() << ", " << CandidatesSize << " HB candidates";
        if(do_deg1_elements)
            verboseOutput() << ", " << CandidatesSize << " deg1 vectors";
        verboseOutput() << " accumulated." << endl;
    }
    
    if(!keep_triangulation){
        // Triangulation.clear();
        // #pragma omp critical(FREESIMPL)
        FreeSimpl.splice(FreeSimpl.begin(),Triangulation);
        TriangulationSize=0;
    }
    
    } // TriangulationSize

    // intermediate reduction //TODO better integration
    if (do_Hilbert_basis && CandidatesSize >= IntermedRedBoundHB) {                    // 2000000
        if (!isComputed(ConeProperty::SupportHyperplanes)) {
            if (verbose) {
                verboseOutput() << "**** Computing support hyperplanes for intermediate reduction:" << endl;
            }
            Full_Cone copy((*this).Generators); //TODO give more information
            copy.compute_support_hyperplanes();
            Support_Hyperplanes.splice(Support_Hyperplanes.begin(),copy.Support_Hyperplanes);
            is_Computed.set(ConeProperty::SupportHyperplanes);
            do_all_hyperplanes = false;
        }
        global_reduction();
        Candidates.splice(Candidates.begin(), Hilbert_Basis);
        CandidatesSize = 0; //TODO is not 0
        // ReportPyr=true;
    }

}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::primal_algorithm(){

    // set_degrees(); // now done in compute()
    // sort_gens_by_degree();
    

    if (!is_pyramid) {
        SimplexEval = vector< SimplexEvaluator<Integer> >(omp_get_max_threads(),SimplexEvaluator<Integer>(*this));
        // this is used when we want to make intermediate reductions
        if (do_Hilbert_basis)
        for (size_t i = 0; i <nr_gen; i++) {
            // cout << gen_levels[i] << " ** " << Generators[i];
            if(!inhomogeneous || gen_levels[i]<=1)
                Candidates.push_front(Generators[i]);
        }
    }

    /***** Main Work is done in build_top_cone() *****/
    build_top_cone();  // evaluates if keep_triangulation==false
    /***** Main Work is done in build_top_cone() *****/


    extreme_rays_and_deg1_check();
    if(!pointed) return;

    if (keep_triangulation) {
        if (isComputed(ConeProperty::Grading) && !deg1_generated) {
            deg1_triangulation = false;
        }
        evaluate_triangulation();
    }
    FreeSimpl.clear();

    // collect accumulated data from the SimplexEvaluators
    if(!is_pyramid) {
        for (int zi=0; zi<omp_get_max_threads(); zi++) {
            detSum += SimplexEval[zi].getDetSum();
            multiplicity += SimplexEval[zi].getMultiplicitySum(); 
            if (do_h_vector) {
                Hilbert_Series += SimplexEval[zi].getHilbertSeriesSum();
            }
        }
    }
    
    if (do_triangulation || do_partial_triangulation) {
        is_Computed.set(ConeProperty::TriangulationSize,true);
        if (do_evaluation) {
            is_Computed.set(ConeProperty::TriangulationDetSum,true);
        }
    }
    if (do_triangulation && do_evaluation && isComputed(ConeProperty::Grading))
        is_Computed.set(ConeProperty::Multiplicity,true);
    if (do_Hilbert_basis) {
        global_reduction();
        Hilbert_Basis.sort(); Hilbert_Basis.unique(); // TODO make it smarter!
        is_Computed.set(ConeProperty::HilbertBasis,true);
        check_integrally_closed();
        if (isComputed(ConeProperty::Grading)) {
            select_deg1_elements();
            check_deg1_hilbert_basis();
        }
    }
    
    if (do_deg1_elements) {
        Deg1_Elements.splice(Deg1_Elements.begin(), Candidates);
        for(size_t i=0;i<nr_gen;i++)
            if(in_triang[i] && v_scalar_product(Grading,Generators[i])==1)
                Deg1_Elements.push_front(Generators[i]);
        Deg1_Elements.sort();
        Deg1_Elements.unique();  //TODO sort, unique needed?
        is_Computed.set(ConeProperty::Deg1Elements,true);
    }
    if (do_h_vector) {
        Hilbert_Series.simplify();
        is_Computed.set(ConeProperty::HilbertSeries);
    }
    if(do_Stanley_dec){
        is_Computed.set(ConeProperty::StanleyDec);
    }

}

   
//---------------------------------------------------------------------------
// Normaliz modes (public)
//---------------------------------------------------------------------------

// pure dualization
template<typename Integer>
void Full_Cone<Integer>::dualize_cone() {  
    compute_support_hyperplanes();
    // reset_tasks();
}

// check the do_* bools, they must be set in advance
// this method (de)activate them according to dependencies between them
template<typename Integer>
void Full_Cone<Integer>::do_vars_check() {

    if (do_default_mode) {
        do_Hilbert_basis = true;
        do_h_vector = true;
    }

    // activate implications
    if (do_Stanley_dec)     keep_triangulation = true;
    if (keep_triangulation) do_determinants = true;
    if (do_multiplicity)    do_determinants = true;
    if (do_determinants)    do_triangulation = true;
    if (do_h_vector)        do_triangulation = true;
    if (do_deg1_elements)   do_partial_triangulation = true;
    if (do_Hilbert_basis)   do_partial_triangulation = true;
    // activate 
    do_only_multiplicity = do_determinants;
    if (do_Stanley_dec || do_h_vector || do_deg1_elements || do_Hilbert_basis) {
        do_only_multiplicity = false;
        do_evaluation = true;
    }
    if (do_determinants)    do_evaluation = true;

    if (do_triangulation)   do_partial_triangulation = false;
    if (do_Hilbert_basis)   do_deg1_elements = false; //they will be extracted later
}


// general purpose compute method
// do_* bools must be set in advance, this method does sanity checks for it
// if no bool is set it does support hyperplanes and extreme rays
template<typename Integer>
void Full_Cone<Integer>::compute() {
    do_vars_check();

    if (!do_triangulation && !do_partial_triangulation)
        support_hyperplanes();
    else{
        // set needed do_ vars
        if (do_Hilbert_basis||do_deg1_elements||do_h_vector)
        do_evaluation = true;
        
        if(inhomogeneous)
            set_levels();
        
        // look for a grading if it is needed
        find_grading();
        if(isComputed(ConeProperty::IsPointed) && !pointed) return;
        
        if (!isComputed(ConeProperty::Grading))
            disable_grading_dep_comp();
            
        if(inhomogeneous){
            find_level0_dim();
            cout << "level0 dim " << level0_dim << endl;
        }

        set_degrees();
        sort_gens_by_degree();

        
        if(ExcludedFaces.nr_of_rows()>0){
            if(!do_h_vector && !do_Stanley_dec){
                errorOutput() << endl << "Warning: exluded faces, but no h-vector computation or Stanley decomposition" << endl
                    << "Therefore excluded faces will be ignored" << endl;           
            }
            else {
                do_excluded_faces=true;
                prepare_inclusion_exclusion();
            }
        }
            
        if(do_approximation && !deg1_generated){
            if(!isComputed(ConeProperty::ExtremeRays) || !isComputed(ConeProperty::SupportHyperplanes))
                support_hyperplanes();
            if(verbose)
                verboseOutput() << "Approximating rational by lattice polytope" << endl;
            compute_deg1_elements_via_approx();
            if(do_triangulation){
                do_deg1_elements=false;
                do_partial_triangulation=false;
                do_only_multiplicity = do_determinants;
                primal_algorithm();            
            }
        }
        else
            primal_algorithm();
            
        if(inhomogeneous){
            find_module_rank();
            cout << "module rank " << module_rank << endl;
        }
        
    }
}

// -1
template<typename Integer>
void Full_Cone<Integer>::compute_deg1_elements_via_approx() {

    if (!isComputed(ConeProperty::Grading)){
        support_hyperplanes(); // the only thing we can do now
        return;
    }

    Full_Cone C_approx(latt_approx()); // latt_approx computes a matrix of generators
    C_approx.do_deg1_elements=true;    // for supercone C_approx that is generated in degree 1
    if(verbose)
        verboseOutput() << "Computing deg 1 elements in approximating cone" << endl;
    C_approx.compute();
    if(!C_approx.contains(*this) || Grading!=C_approx.Grading){
        errorOutput() << "Wrong approximating cone. Fatal error. PLEASE CONTACT THE AUTHORS" << endl;
        throw FatalException();
    }

    if(verbose)
        verboseOutput() << "Sum of dets of simplicial cones evaluated in approximation = " << C_approx.detSum << endl;

    if(verbose)
        verboseOutput() << "Returning to original cone" << endl;
    // compute_support_hyperplanes();  // we need them to selct the deg 1 elements in C
    if(verbose)
        verboseOutput() << "Selecting deg 1 elements from approximating cone" << endl;
    select_deg1_elements(C_approx);
    
    if(verbose)
        verboseOutput() << Deg1_Elements.size() << " deg 1 elements found" << endl;
}


// -s
template<typename Integer>
void Full_Cone<Integer>::support_hyperplanes() {
    // recursion_allowed=true;
    compute_support_hyperplanes();
    extreme_rays_and_deg1_check();
    // reset_tasks();
}

/*template<typename Integer>  // now in constructor
void Full_Cone<Integer>::dual_mode() {

} */

//---------------------------------------------------------------------------
// Checks and auxiliary algorithms
//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::extreme_rays_and_deg1_check() {
    check_pointed();
    if(!pointed) return;
    compute_extreme_rays();
    deg1_check();
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::find_grading(){
    
    if(inhomogeneous) // in the inhomogeneous case we do not allow implicit grading
        return;

    deg1_check(); // trying to find grading under which all generators have the same degree
    if (!isComputed(ConeProperty::Grading) && (do_multiplicity || do_deg1_elements || do_h_vector)) {
        if (!isComputed(ConeProperty::ExtremeRays)) {
            if (verbose) {
                verboseOutput() << "Cannot find grading s.t. all generators have the same degree! Computing Extreme rays first:" << endl;
            }
            compute_support_hyperplanes();
            extreme_rays_and_deg1_check();
            if(!pointed) return;

            // We keep the SupportHyperplanes, so we do not need to recompute them
            // for the last generator, and use them to make a global reduction earlier
            do_all_hyperplanes = false;
            for(size_t i=0;i<nr_gen;i++)
                in_triang[i]=false;
            nextGen = 0;
        }
    }
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::find_level0_dim(){  // TO DO: use information of find_module_rank

    if(!isComputed(ConeProperty::Generators)){
        errorOutput() << "Missing Generators. THIS SOULD NOT HAPPEN!"  << endl;
        throw BadInputException();   
    }
    
    Matrix<Integer> Help(nr_gen,dim);
    for(size_t i=0; i<nr_gen;++i)
        if(gen_levels[i]==0)
            Help[i]=Generators[i];
    level0_dim=Help.rank();
    is_Computed.set(ConeProperty::RecessionRank);
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::find_module_rank(){

     if(level0_dim==dim-1){
        module_rank=1;
        is_Computed.set(ConeProperty::ModuleRank);
        return;
     }      
    
    if(!isComputed(ConeProperty::HilbertBasis))
        return;
        
    Matrix<Integer> Help(nr_gen,dim);
    for(size_t i=0; i<nr_gen;++i)
        if(gen_levels[i]==0)
            Help[i]=Generators[i];
        
    Matrix<Integer> QuotientEmbedding=Help.kernel();
    
    set<vector<Integer> > Quotient;
    vector<Integer> v;
    
    typename list<vector<Integer> >::iterator h;
    
    for(h=Hilbert_Basis.begin();h!=Hilbert_Basis.end();++h){
        v=QuotientEmbedding.MxV(*h);
        bool zero=true;
        for(size_t j=0;j<v.size();++j)
            if(v[j]!=0){
                zero=false;
                break;
            }
        if(!zero)
            Quotient.insert(v);
    }
    
    module_rank=Quotient.size();
    is_Computed.set(ConeProperty::ModuleRank);

}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::find_grading_inhom(){

    if(Grading.size()==0 || Truncation.size()==0){
        errorOutput() << "Cannot find grading in the inhomogeneous case! THIS SHOULD NOT HAPPEN." << endl;
         throw BadInputException(); 
    }

    bool first=true;
    Integer level,degree,quot=0,min_quot=0;
    for(size_t i=0;i<nr_gen;++i){
        level=v_scalar_product(Truncation,Generators[i]);
        if(level==0)
            continue;
        degree=v_scalar_product(Grading,Generators[i]);
        quot=degree/level;
        // cout << Generators[i];
        // cout << "*** " << degree << " " << level << " " << quot << endl;
        if(level*quot>=degree)
            quot--;
        if(first){
            min_quot=quot;
            first=false;
        }
        if(quot<min_quot)
            min_quot=quot;
        // cout << "+++ " << min_quot << endl;
    }
    shift=-min_quot;
    for(size_t i=0;i<dim;++i) // under this grading all generators have positive degree
        Grading[i]=Grading[i]+shift*Truncation[i];
        
    shift--;  // coorection for the Hilbert series computation to have it start in degree 0
    
    is_Computed.set(ConeProperty::Shift);
        
    cout << "Shlft " << shift << endl;  
    cout << Grading;
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::set_degrees() {
    if(gen_degrees.size()==0 && isComputed(ConeProperty::Grading)) // now we set the degrees
    {
        gen_degrees.resize(nr_gen);
        vector<Integer> gen_degrees_Integer=Generators.MxV(Grading);
        for (size_t i=0; i<nr_gen; i++) {
            if (gen_degrees_Integer[i] < 1) {
                errorOutput() << "Grading gives non-positive value " << gen_degrees_Integer[i] << " for generator " << i+1 << "." << endl;
                throw BadInputException();
            }
            gen_degrees[i] = explicit_cast_to_long(gen_degrees_Integer[i]);
        }
    }
    
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::set_levels() {
    if(inhomogeneous && Truncation.size()!=dim){
        errorOutput() << "Truncsation not defined in inhomogeneous case. THIS SHOULD NOT HAPPEN !" << endl;
        throw BadInputException();
    }    
    
    // cout <<"trunc " << Truncation;

    if(gen_levels.size()==0) // now we compute the levels
    {
        gen_levels.resize(nr_gen);
        vector<Integer> gen_levels_Integer=Generators.MxV(Truncation);
        for (size_t i=0; i<nr_gen; i++) {
            if (gen_levels_Integer[i] < 0) {
                errorOutput() << "Truncation gives non-positive value " << gen_levels_Integer[i] << " for generator " << i+1 << "." << endl;
                errorOutput() << "THIS SHOULD NOT HAPPEN !" << endl;
                throw BadInputException();
            }
            gen_levels[i] = explicit_cast_to_long(gen_levels_Integer[i]);
            // cout << "Gen " << Generators[i];
            // cout << "level " << gen_levels[i] << endl << "----------------------" << endl;
        }
    }
    
}
    
//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::sort_gens_by_degree() {
    if(deg1_extreme_rays)  // gen_degrees.size()==0 || 
        return;
    
    list<vector<Integer> > genList;
    vector<Integer> v;
    if(inhomogeneous)
        v.resize(dim+4);
    else
        v.resize(dim+3);
    vector<Integer> w(dim);
    unsigned long i,j;
    
    for(i=0;i<nr_gen;i++){
        if(isComputed(ConeProperty::Grading))
            v[0]=gen_degrees[i];
        else{
            v[0]=0;
            for(j=0;j<dim;++j)
                v[0]+=Iabs(Generators[i][j]);       
        }
                
        v[1]=i;                // keep the input order as far as possible
        w=Generators[i];
        for(j=0;j<dim;j++)
            v[j+2]=w[j];
        v[dim+2]=0;
        if(Extreme_Rays[i]) // after sorting we must recover the extreme rays
            v[dim+2]=1;
        if(inhomogeneous)
            v[dim+3]=gen_levels[i];
        genList.push_back(v);
    }
    genList.sort();
    
    i=0;
    typename list<vector<Integer> >::iterator g=genList.begin();
    for(;g!=genList.end();++g){
        v=*g;
        if(isComputed(ConeProperty::Grading))
            gen_degrees[i]=explicit_cast_to_long<Integer>(v[0]);
        if(inhomogeneous)
            gen_levels[i]=explicit_cast_to_long<Integer>(v[dim+3]);
        Extreme_Rays[i]=false;
        if(v[dim+2]>0)
            Extreme_Rays[i]=true;
        for(j=0;j<dim;j++)
            w[j]=v[j+2];
        Generators[i]=w;
        i++;
    }
    
    if (verbose) {
        verboseOutput() << endl << "Degrees after sort" << endl;
        verboseOutput() << count_in_map<long,long>(gen_degrees);
    }
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::compute_support_hyperplanes(){
    if(isComputed(ConeProperty::SupportHyperplanes))
        return;

    bool save_tri      = do_triangulation;
    bool save_part_tri = do_partial_triangulation;
    do_triangulation         = false;
    do_partial_triangulation = false;

    build_top_cone();

    do_triangulation         = save_tri;
    do_partial_triangulation = save_part_tri;
}

//---------------------------------------------------------------------------

template<typename Integer>
Simplex<Integer> Full_Cone<Integer>::find_start_simplex() const {

    if (isComputed(ConeProperty::ExtremeRays)) {
        vector<key_t> marked_extreme_rays(0);
        for (size_t i=0; i<nr_gen; i++) {
            if (Extreme_Rays[i])
                marked_extreme_rays.push_back(i);
        }
        vector<key_t> key_extreme = Generators.submatrix(Extreme_Rays).max_rank_submatrix_lex(dim);
        assert(key_extreme.size() == dim);
        vector<key_t> key(dim);
        for (key_t i=0; i<dim; i++) {
            key[i] = marked_extreme_rays[key_extreme[i]];
        }
        return Simplex<Integer>(key, Generators);
    } 
    else {
    // assert(Generators.rank()>=dim); 
        return Simplex<Integer>(Generators);
    }
}

//---------------------------------------------------------------------------

template<typename Integer>
Matrix<Integer> Full_Cone<Integer>::select_matrix_from_list(const list<vector<Integer> >& S,
                                   vector<size_t>& selection){

    sort(selection.begin(),selection.end());
    assert(selection.back()<S.size());
    size_t i=0,j=0;
    size_t k=selection.size();
    Matrix<Integer> M(selection.size(),S.front().size());
    typename list<vector<Integer> >::const_iterator ll=S.begin();
    for(;ll!=S.end()&&i<k;++ll){
        if(j==selection[i]){
            M[i]=*ll;
            i++;
        }
        j++;
    }
    return M;
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::compute_extreme_rays(){

    if (isComputed(ConeProperty::ExtremeRays))
        return;
    assert(isComputed(ConeProperty::SupportHyperplanes));

    if(dim*Support_Hyperplanes.size() < nr_gen)
         compute_extreme_rays_rank();
    else
         compute_extreme_rays_compare();
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::compute_extreme_rays_rank(){

        size_t i,j;
    typename list<vector<Integer> >::iterator s;
    vector<size_t> gen_in_hyperplanes;
    gen_in_hyperplanes.reserve(Support_Hyperplanes.size());
    Matrix<Integer> M;
    
    for(i=0;i<nr_gen;++i){
        Extreme_Rays[i]=false;
        if (isComputed(ConeProperty::Triangulation) && !in_triang[i])
            continue;
        j=0;
        gen_in_hyperplanes.clear();
        for(s=Support_Hyperplanes.begin();s!=Support_Hyperplanes.end();++s){
            if(v_scalar_product(Generators[i],*s)==0)
                gen_in_hyperplanes.push_back(j);
            j++;
        }
        if(gen_in_hyperplanes.size()< dim-1)
            continue;
        M=select_matrix_from_list(Support_Hyperplanes,gen_in_hyperplanes);
        if(M.rank_destructive()>=dim-1)
            Extreme_Rays[i]=true;   
    }

    is_Computed.set(ConeProperty::ExtremeRays);
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::compute_extreme_rays_compare(){

    size_t i,j,k,l,t;
    // Matrix<Integer> SH=getSupportHyperplanes().transpose();
    // Matrix<Integer> Val=Generators.multiplication(SH);
    size_t nc=Support_Hyperplanes.size();
    
    vector<vector<bool> > Val(nr_gen);
    for (i=0;i<nr_gen;++i)
       Val[i].resize(nc);
        
    // Attention: in this routine Val[i][j]==0, i.e. false, indicates that
    // the i-th generator is contained in the j-th support hyperplane
    
    vector<key_t> Zero(nc);
    vector<key_t> nr_zeroes(nr_gen);
    typename list<vector<Integer> >::iterator s;

    for (i = 0; i <nr_gen; i++) {
        if (isComputed(ConeProperty::Triangulation) && !in_triang[i]) {
            Extreme_Rays[i]=false;
            continue;
        }
        k=0;
        Extreme_Rays[i]=true;
        s=Support_Hyperplanes.begin();
        for (j = 0; j <nc; ++j,++s) {
            if (v_scalar_product(Generators[i],*s)==0) {
                k++;
                Val[i][j]=false;                
            }
            else
                Val[i][j]=true;  
        }
        nr_zeroes[i]=k;
        if (k<dim-1||k==nc)  // not contained in enough facets or in all (0 as generator)
            Extreme_Rays[i]=false;
    }

    for (i = 0; i <nr_gen; i++) {
        if(!Extreme_Rays[i])  // already known to be non-extreme
            continue;

        k=0;
        for (j = 0; j <nc; j++) {
            if (Val[i][j]==false) {
                Zero[k]=j;
                k++;
            }
        }

        for (j = 0; j <nr_gen; j++) {
            if (i!=j && Extreme_Rays[j]                // not compare with itself or a known nonextreme ray
                     && nr_zeroes[i]<nr_zeroes[j]) {   // or something whose zeroes cannot be a superset
                l=0;
                for (t = 0; t < nr_zeroes[i]; t++) {
                    if (Val[j][Zero[t]]==false)
                        l++;
                    if (l>=nr_zeroes[i]) {
                        Extreme_Rays[i]=false;
                        break;
                    }
                }
            }
        }
    }

    is_Computed.set(ConeProperty::ExtremeRays);
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::select_deg1_elements() { // from the Hilbert basis

    if(inhomogeneous)
        return;
    typename list<vector<Integer> >::iterator h = Hilbert_Basis.begin();
    for(;h!=Hilbert_Basis.end();h++)
        if(v_scalar_product(Grading,*h)==1)
            Deg1_Elements.push_back(*h);
    is_Computed.set(ConeProperty::Deg1Elements,true);
}

//---------------------------------------------------------------------------

template<typename Integer>
bool Full_Cone<Integer>::contains(const vector<Integer>& v) {
    typename list<vector<Integer> >::iterator s;
    for(s= Support_Hyperplanes.begin();s!= Support_Hyperplanes.end();++s)
        if(v_scalar_product(*s,v)<0)
            return(false);
    return(true);
}
//---------------------------------------------------------------------------

template<typename Integer>
bool Full_Cone<Integer>::contains(const Full_Cone& C) {
    for(size_t i=0;i<C.nr_gen;++i)
        if(!contains(C.Generators[i])){
            cerr << "Missing generator " << C.Generators[i] << endl;
            return(false);
    }
    return(true);
}
//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::select_deg1_elements(const Full_Cone& C) {  // from vectors computed in 
                                                              // the auxiliary cone C
    assert(isComputed(ConeProperty::SupportHyperplanes));
    assert(C.isComputed(ConeProperty::Deg1Elements));
    typename list<vector<Integer> >::const_iterator h = C.Deg1_Elements.begin();
    for(;h!=C.Deg1_Elements.end();++h){
        if(contains(*h))
            Deg1_Elements.push_back(*h);
    }
    is_Computed.set(ConeProperty::Deg1Elements,true);
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::check_pointed() {
    assert(isComputed(ConeProperty::SupportHyperplanes));
    if (isComputed(ConeProperty::IsPointed))
        return;
    Matrix<Integer> SH = getSupportHyperplanes();
    pointed = (SH.rank_destructive() == dim);
    is_Computed.set(ConeProperty::IsPointed);
}


//---------------------------------------------------------------------------
template<typename Integer>
void Full_Cone<Integer>::disable_grading_dep_comp() {

    if (do_multiplicity || do_deg1_elements || do_h_vector) {
        if (do_default_mode) {
            if (verbose)
                verboseOutput() << "No grading specified and cannot find one. "
                                << "Disabling some computations!" << endl;
            do_deg1_elements = false;
            do_h_vector = false;
        } else {
            errorOutput() << "No grading specified and cannot find one. "
                          << "Cannot compute some requested properties!" << endl;
            throw BadInputException();
        }
    }
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::deg1_check() {

    if(inhomogeneous)  // deg 1 check isabled since it makes no sense in this case
        return;
        
    if (!isComputed(ConeProperty::Grading)          // we still need it and
     && !isComputed(ConeProperty::IsDeg1ExtremeRays)) { // we have not tried it
        if (isComputed(ConeProperty::ExtremeRays)) {
            Matrix<Integer> Extreme=Generators.submatrix(Extreme_Rays);
            Grading = Extreme.find_linear_form();
            if (Grading.size() == dim) {
                is_Computed.set(ConeProperty::Grading);
            } else {
                deg1_extreme_rays = false;
                is_Computed.set(ConeProperty::IsDeg1ExtremeRays);
            }
        } else // extreme rays not known
        if (!isComputed(ConeProperty::IsDeg1Generated)) {
            Grading = Generators.find_linear_form();
            if (Grading.size() == dim) {
                is_Computed.set(ConeProperty::Grading);
            } else {
                deg1_generated = false;
                is_Computed.set(ConeProperty::IsDeg1Generated);
            }
        }
    }

    //now we hopefully have a grading

    if (!isComputed(ConeProperty::Grading)) {
        if (isComputed(ConeProperty::ExtremeRays)) {
            // there is no hope to find a grading later
            deg1_generated = false;
            is_Computed.set(ConeProperty::IsDeg1Generated);
            deg1_extreme_rays = false;
            is_Computed.set(ConeProperty::IsDeg1ExtremeRays);
            disable_grading_dep_comp();
        }
        return; // we are done
    }
    
    set_degrees();
        
    if (!isComputed(ConeProperty::IsDeg1Generated)) {
        deg1_generated = true;
        for (size_t i = 0; i < nr_gen; i++) {
            if (gen_degrees[i] != 1) {
                deg1_generated = false;
                break;
            }
        }
        is_Computed.set(ConeProperty::IsDeg1Generated);
        if (deg1_generated) {
            deg1_extreme_rays = true;
            is_Computed.set(ConeProperty::IsDeg1ExtremeRays);
        }
    }
    if (!isComputed(ConeProperty::IsDeg1ExtremeRays)
      && isComputed(ConeProperty::ExtremeRays)) {
        deg1_extreme_rays = true;
        for (size_t i = 0; i < nr_gen; i++) {
            if (Extreme_Rays[i] && gen_degrees[i] != 1) {
                deg1_extreme_rays = false;
                break;
            }
        }
        is_Computed.set(ConeProperty::IsDeg1ExtremeRays);
    }
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::check_deg1_hilbert_basis() {
    if (isComputed(ConeProperty::IsDeg1HilbertBasis) || inhomogeneous)
        return;

    if ( !isComputed(ConeProperty::Grading) || !isComputed(ConeProperty::HilbertBasis)) {
        errorOutput() << "WARNING: unsatisfied preconditions in check_deg1_hilbert_basis()!" <<endl;
        return;
    }
    
    if (isComputed(ConeProperty::Deg1Elements)) {
        deg1_hilbert_basis = (Deg1_Elements.size() == Hilbert_Basis.size());
    } else {
        deg1_hilbert_basis = true;
        typename list< vector<Integer> >::iterator h;
        for (h = Hilbert_Basis.begin(); h != Hilbert_Basis.end(); ++h) {
            if (v_scalar_product((*h),Grading)!=1) {
                deg1_hilbert_basis = false;
                break;
            }
        }
    }
    is_Computed.set(ConeProperty::IsDeg1HilbertBasis);
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::check_integrally_closed() {
    if (isComputed(ConeProperty::IsIntegrallyClosed))
        return;

    if ( !isComputed(ConeProperty::HilbertBasis)) {
        errorOutput() << "WARNING: unsatisfied preconditions in check_integrally_closed()!" <<endl;
        return;
    }
    integrally_closed = false;
    if (Hilbert_Basis.size() <= nr_gen) {
        integrally_closed = true;
        typename list< vector<Integer> >::iterator h;
        for (h = Hilbert_Basis.begin(); h != Hilbert_Basis.end(); ++h) {
            integrally_closed = false;
            for (size_t i=0; i< nr_gen; i++) {
                if ((*h) == Generators[i]) {
                    integrally_closed = true;
                    break;
                }
            }
            if (!integrally_closed) {
                break;
            }
        }
    }
    is_Computed.set(ConeProperty::IsIntegrallyClosed);
}

//---------------------------------------------------------------------------

// Computes the generators of a supercone approximating "this" by a cone over a lattice polytope
template<typename Integer>
Matrix<Integer> Full_Cone<Integer>::latt_approx() {
    assert(isComputed(ConeProperty::Grading));
    assert(isComputed(ConeProperty::ExtremeRays));
    Matrix<Integer> G(1,dim);
    G[0]=Grading;
    
    Lineare_Transformation<Integer> NewBasis = Transformation(G); // gives a new basis in which the grading is a coordinate
    Matrix<Integer> U=NewBasis.get_right();   // the basis elements are the columns of U

    Integer dummy_denom;                             
    vector<Integer> dummy_diag(dim); 
    Matrix<Integer> T=invert(U,dummy_diag,dummy_denom);       // T is the coordinate transformation
                                                            // to the new basis: v --> Tv (in this case)
                                                    // for which the grading is the FIRST coordinate

    assert(dummy_denom==1);  // for safety 

    // It can happen that -Grading has become the first row of T, but we want Grading. If necessary we replace the
    // first row by its negative, and correspondingly the first column of U by its negative

    if(T[0]!=Grading){
        for(size_t i=0;i<dim;++i){
            U[i][0]*=-1;
            T[0][i]*=-1;
        }
    }
    assert(T[0] == Grading);
    
    list<vector<Integer> > L; // collects the generators of the approximating cone
    for(size_t i=0;i<nr_gen;++i){
        if(Extreme_Rays[i]){
            list<vector<Integer> > approx;
            approx_simplex(T.MxV(Generators[i]),approx);
            L.splice(L.end(),approx);
        }
    }
    
    Matrix<Integer> M=Matrix<Integer>(L);
    
    for(size_t j=0;j<M.nr_of_rows();++j)  // reverse transformation
        M[j]=U.MxV(M[j]);
    
    return(M);
} 

//---------------------------------------------------------------------------       
template<typename Integer>
void Full_Cone<Integer>::prepare_inclusion_exclusion(){
 
    vector<boost::dynamic_bitset<> > GensInExcl(ExcludedFaces.nr_of_rows()); // indicates
                                   // which generators lie in the excluded faces

    for(size_t j=0;j<ExcludedFaces.nr_of_rows();++j){ // now we produce these indicators
        bool first_neq_0=true;           // and check whether the inear forms in ExcludedFaces
        bool non_zero=false;             // have the cone on one side
        GensInExcl[j].resize(nr_gen,false);
        for(size_t i=0; i< nr_gen;++i){
            Integer test=v_scalar_product(ExcludedFaces[j],Generators[i]);
            if(test==0){
                GensInExcl[j].set(i);
                continue;
            }
            non_zero=true;
            if(first_neq_0){
                first_neq_0=false;
                if(test<0){
                    for(size_t k=0;k<dim;++k)  // replace linear dorm by its negative
                        ExcludedFaces[j][k]*=-1;  // to get cone in positive halfspace 
                    test*=-1;                     // (only for error check)
                }    
             }
            if(test<0){
                errorOutput() << "Fatal error: excluded hyperplane does not define a face" << endl;
                throw FatalException();
            }            
                
        }
        if(!non_zero){  // not impossible if the hyperplane contains the vector space spanned by the cone
            errorOutput() << "Fatal error: excluded face contains the full cone" << endl;
            throw FatalException();
        }
    }
    
    vector<bool> essential(ExcludedFaces.nr_of_rows(),true);
    bool remove_one=false;
    for(size_t i=0;i<essential.size();++i)
        for(size_t j=i+1;j<essential.size();++j){
            if(GensInExcl[j].is_subset_of(GensInExcl[i])){
                essential[j]=false;
                remove_one=true;
                continue;
            }
            if(GensInExcl[i].is_subset_of(GensInExcl[j])){
                essential[i]=false;
                remove_one=true;
            }
        }
    if(remove_one){
        Matrix<Integer> Help(0,dim);
        for(size_t i=0;i<essential.size();++i)
            if(essential[i])
                Help.append(ExcludedFaces[i]);
        ExcludedFaces=Help;
    }
    is_Computed.set(ConeProperty::ExcludedFaces);
    
    vector< pair<boost::dynamic_bitset<> , long> > InExScheme;  // now we produce the formal 
    boost::dynamic_bitset<> all_gens(nr_gen);             // inclusion-exclusion scheme
    all_gens.set();                         // by forming all intersections of
                                           // excluded faces
    InExScheme.push_back(pair<boost::dynamic_bitset<> , long> (all_gens, 1));
    size_t old_size=1;
    
    for(size_t i=0;i<ExcludedFaces.nr_of_rows();++i){
        for(size_t j=0;j<old_size;++j)
            InExScheme.push_back(pair<boost::dynamic_bitset<> , long>
                   (InExScheme[j].first & GensInExcl[i], -InExScheme[j].second));
        old_size*=2;
    }
    
    vector<pair<boost::dynamic_bitset<>, long> >::iterator G;       
    
    InExScheme.erase(InExScheme.begin()); // remove full cone
    
    // map<boost::dynamic_bitset<>, long> InExCollect;
    map<boost::dynamic_bitset<>, long>::iterator F;
    
    for(size_t i=0;i<old_size-1;++i){               // we compactify the list of faces
        F=InExCollect.find(InExScheme[i].first);    // obtained as intersections
        if(F!=InExCollect.end())                    // by listing each face only once
            F->second+=InExScheme[i].second;        // but with the right multiplicity
        else
            InExCollect.insert(InExScheme[i]);
    }
     
    for(F=InExCollect.begin();F!=InExCollect.end();){   // faces with multiplicity 0
        if(F->second==0)                                 // can be erased
            InExCollect.erase(F++);
        else{
            ++F;
        }    
    }
     
    if(verbose){
        verboseOutput() << endl;
        verboseOutput() << "in_ex_data" << endl;
        verboseOutput() << InExCollect.size() << endl;
        for(F=InExCollect.begin();F!=InExCollect.end();++F){
            vector<key_t> key;
            for(size_t i=0;i<nr_gen;++i)
                if(F->first.test(i))
                    key.push_back(i);
            verboseOutput() << key.size() << " ";
            for(size_t i=0;i<key.size();++i)
                verboseOutput() << key[i]+1 << " ";
            verboseOutput() << F->second << endl;  
        }
         
        verboseOutput() << "--------------------------------------------" << endl; 
         
        verboseOutput() << "InEx complete, " << InExCollect.size() << " faces involved" << endl;
    }
     
    is_Computed.set(ConeProperty::InclusionExclusionData);
} 

//---------------------------------------------------------------------------
// Global reduction
//---------------------------------------------------------------------------

// Returns true if new_element is reducible versus the elements in Irred
template<typename Integer>
bool Full_Cone<Integer>::is_reducible(list< vector<Integer>* >& Irred, const vector< Integer >& new_element){
    size_t i;
    size_t s=Support_Hyperplanes.size();
    // new_element can be longer than dim (it has one extra entry for the norm)
    // the scalar product function just takes the first dim entries
    vector <Integer> scalar_product=l_multiplication(Support_Hyperplanes,new_element);
    typename list< vector<Integer>* >::iterator j;
    vector<Integer> *reducer;
    for (j =Irred.begin(); j != Irred.end(); j++) {
        reducer=(*j);
        for (i = 0; i < s; i++) {
            if ((*reducer)[i]>scalar_product[i]){
                break;
            }
        }
        if (i==s) {
            //found a "reducer" and move it to the front
            Irred.push_front(*j);
            Irred.erase(j);
            return true;
        }
    }
    return false;
}

//---------------------------------------------------------------------------

// reduce the Candidates against itself and stores the remaining elements in Hilbert_Basis */
template<typename Integer>
void Full_Cone<Integer>::global_reduction() {
    Integer norm;
    
    list <vector<Integer> > HB;
    typename list <vector<Integer> >::iterator c;
    
/*    for (size_t i = 0; i <nr_gen; i++) {
        if (in_triang[i])
            Candidates.push_front(Generators[i]);
    }*/ //now done earlier
/*    if(verbose) verboseOutput()<<"sorting the candidates... "<<flush;
    Candidates.sort();
    if(verbose) verboseOutput()<<"make them unique... "<<flush;
    Candidates.unique();
    if(verbose) verboseOutput()<<"done."<<endl;
*/  // Duplicates are avoided or removed earlier
    if (nr_gen == dim) { // cone is simplicial, therefore no global reduction is necessary
        Hilbert_Basis.splice(Hilbert_Basis.end(), Candidates);
        if (verbose) {
            verboseOutput()<<"Cone is simplicial, no global reduction necessary."<<endl;
            verboseOutput()<<Hilbert_Basis.size()<< " Hilbert Basis elements"<<endl;
        }
        return;
    }
    

    vector<Integer> degree_function=compute_degree_function();

    c = Candidates.begin();
    size_t cpos = 0;
    size_t csize=Candidates.size();
    
    if(verbose) {
        verboseOutput()<<"computing the degrees of the candidates... "<<flush;
    }
    //go over candidates: do single scalar product and save it at the end of the candidate
    //for (c = Candidates.begin(); c != Candidates.end(); c++) 
    vector<Integer> scalar_product;
    for (size_t j=0; j<csize; ++j) {
        for(;j > cpos; ++cpos, ++c) ;
        for(;j < cpos; --cpos, --c) ;

        norm=v_scalar_product(degree_function,(*c));
        c->reserve(dim+1);
        c->push_back(norm);

    }
    if(verbose) {
        verboseOutput()<<"sorting the list... "<<endl;
    }
    Candidates.sort(compare_last<Integer>);
    if (verbose) {
        verboseOutput()<< csize <<" candidate vectors sorted."<<endl;
    }
    
    // do global reduction
    list< vector<Integer> > HBtmp;
    Integer norm_crit;
    while ( !Candidates.empty() ) {
        //use norm criterion to find irreducible elements
        c=Candidates.begin();
        norm_crit=(*c)[dim]*2;  //candidates with smaller norm are irreducible
        if ( Candidates.back()[dim] < norm_crit) { //all candidates are irreducible
            if (verbose) {
                verboseOutput()<<Hilbert_Basis.size()+Candidates.size();
                verboseOutput()<<" Hilbert Basis elements of degree <= "<<norm_crit-1<<"; done"<<endl;
            }
            for (; c!=Candidates.end(); ++c) {
                c->pop_back();
            }
            Hilbert_Basis.splice(Hilbert_Basis.end(), Candidates);
            break;
        }
        while ( (*c)[dim] < norm_crit ) { //can't go over the end because of the previous if
            // remove norm
            c->pop_back();
            // push the scalar products to the reducer list
            HBtmp.push_back(l_multiplication(Support_Hyperplanes, *c));
            // and the candidate itself to the Hilbert basis
            Hilbert_Basis.splice(Hilbert_Basis.end(), Candidates, c++);
        }
        csize = Candidates.size();
        if (verbose) {
            verboseOutput()<<Hilbert_Basis.size()<< " Hilbert Basis elements of degree <= "<<norm_crit-1<<"; "<<csize<<" candidates left"<<endl;
        }

        // reduce candidates against HBtmp
        // fill pointer list
        list < vector <Integer>* >  HBpointers;  // used to put "reducer" to the front
        c = HBtmp.begin();
        while (c != HBtmp.end()) {
            HBpointers.push_back(&(*(c++)));
        }

        long VERBOSE_STEPS = 50;      //print | for 2%
        if (verbose && csize>50000) { //print | for 1000 candidates
            VERBOSE_STEPS=csize/1000;
        }
        long step_x_size = csize-VERBOSE_STEPS;
        long counter = 0;
        long steps_done = 0;
        if (verbose) {
            verboseOutput() << "---------+---------+---------+---------+---------+";
            if (VERBOSE_STEPS == 50) {
                verboseOutput() << " (one | per 2%)" << endl;
            } else { 
                verboseOutput() << " (one | per 1000 candidates)" << endl;
            }
        }


        #pragma omp parallel private(c,cpos) firstprivate(HBpointers)
        {
        
        c=Candidates.begin();
        cpos=0;
        #pragma omp for schedule(dynamic)
        for (size_t k=0; k<csize; ++k) {
            for(;k > cpos; ++cpos, ++c) ;
            for(;k < cpos; --cpos, --c) ;
            
            if ( is_reducible(HBpointers, *c) ) {
                (*c)[dim]=-1; //mark as reducible
            }

            if (verbose) {
                #pragma omp atomic
                counter++;

                if (counter*VERBOSE_STEPS >= step_x_size){
                    #pragma omp critical(VERBOSE)
                    while (counter*VERBOSE_STEPS >= step_x_size) {
                        steps_done++;
                        step_x_size += csize;
                        verboseOutput() << "|" <<flush;
                        if(VERBOSE_STEPS > 50 && steps_done%50 == 0) {
                            verboseOutput() << "  " << (steps_done) << "000" << endl;
                        }
                    }
                } //end if
            } //end verbose
        } //end for
        } //end parallel
        if (verbose) verboseOutput() << endl;

        // delete reducible candidates
        c = Candidates.begin();
        while (c != Candidates.end()) {
            if ((*c)[dim]==-1) {
                c = Candidates.erase(c);
            } else {
                ++c;
            }
        }
        HBtmp.clear();
    }

    if (verbose) {
        verboseOutput()<<Hilbert_Basis.size()<< " Hilbert Basis elements"<<endl;
    }
}


//---------------------------------------------------------------------------

/* computes a degree function, s.t. every generator has value >0 */
template<typename Integer>
vector<Integer> Full_Cone<Integer>::compute_degree_function() const {
    size_t i;  
    vector<Integer> degree_function(dim,0);
    if (isComputed(ConeProperty::Grading)) { //use the grading if we have one
        for (i=0; i<dim; i++) {
            degree_function[i] = Grading[i];
        }
    } else { // add hyperplanes to get a degree function
        if(verbose) {
            verboseOutput()<<"computing degree function... "<<flush;
        }
        typename list< vector<Integer> >::const_iterator h;
        for (h=Support_Hyperplanes.begin(); h!=Support_Hyperplanes.end(); ++h) {
            for (i=0; i<dim; i++) {
                degree_function[i]+=(*h)[i];
            }
        } 
        v_make_prime(degree_function);
        if(verbose) {
            verboseOutput()<<"done."<<endl;
        }
    }
    return degree_function;
}

//---------------------------------------------------------------------------

template<typename Integer>
Integer Full_Cone<Integer>::primary_multiplicity() const{
    size_t i,j,k;
    Integer primary_multiplicity=0;
    vector <key_t> key,new_key(dim-1);
    Matrix<Integer> Projection(nr_gen,dim-1);
    for (i = 0; i < nr_gen; i++) {
        for (j = 0; j < dim-1; j++) {
            Projection.write(i,j,Generators[i][j]);
        }
    }
    typename list< vector<Integer> >::const_iterator h;
    typename list< SHORTSIMPLEX<Integer> >::const_iterator t;
    for (h =Support_Hyperplanes.begin(); h != Support_Hyperplanes.end(); ++h){
        if ((*h)[dim-1]!=0) {
            for (t =Triangulation.begin(); t!=Triangulation.end(); ++t){
                key=t->key;
                for (i = 0; i <dim; i++) {
                    k=0;
                    for (j = 0; j < dim; j++) {
                        if (j!=i && Generators[key[j]][dim-1]==1) {
                            if (v_scalar_product(Generators[key[j]],(*h))==0) {
                                k++;
                            }
                        }
                        if (k==dim-1) {
                            for (j = 0; j <i; j++) {
                                new_key[j]=key[j];
                            }
                            for (j = i; j <dim-1; j++) {
                                new_key[j]=key[j+1];
                            }
                            // add the volume of the projected simplex
                            primary_multiplicity +=
                              Projection.submatrix(new_key).vol_destructive();
                        }
                    }
                }
            }
        }
    }
    return primary_multiplicity;
}
//---------------------------------------------------------------------------
// Constructors
//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::reset_tasks(){
    do_triangulation = false;
    do_partial_triangulation = false;
    do_determinants = false;
    do_multiplicity=false;
    do_Hilbert_basis = false;
    do_deg1_elements = false;
    keep_triangulation = false;
    do_Stanley_dec=false;
    do_h_vector=false;
    do_excluded_faces=false;
    do_approximation=false;
    do_default_mode=false;
    
    do_evaluation = false;
    do_only_multiplicity=false;

    nrSimplicialPyr=0;
    totalNrPyr=0;
    is_pyramid = false;
}

//---------------------------------------------------------------------------

template<typename Integer>
Full_Cone<Integer>::Full_Cone(Matrix<Integer> M){ // constructor of the top cone
    dim=M.nr_of_columns();
    if (dim!=M.rank()) {
        error_msg("error: Matrix with rank = number of columns needed in the constructor of the object Full_Cone<Integer>.\nProbable reason: Cone not full dimensional (<=> dual cone not pointed)!");
        throw BadInputException();
    }
    Generators = M;
    nr_gen=Generators.nr_of_rows();
    if (nr_gen != static_cast<size_t>(static_cast<key_t>(nr_gen))) {
        error_msg("Too many generators to fit in range of key_t!");
        throw FatalException();
    }
    //make the generators coprime, remove 0 rows and duplicates
    vector<Integer> gcds = Generators.make_prime();
    bool remove_some = false;
    vector<bool> key(nr_gen, true);
    for (size_t i = 0; i<nr_gen; i++) {
        if (gcds[i] == 0) {
           key[i] = false;
           remove_some = true;
           continue;
        }
        for (size_t j=0; j<i; j++) {
            if (Generators[i] == Generators[j]) {
                key[i] = false;
                remove_some = true;
                break;
            }
        }
    }
    if (remove_some) {
        Generators=Generators.submatrix(key);
        nr_gen=Generators.nr_of_rows();
    }
    multiplicity = 0;
    is_Computed = bitset<ConeProperty::EnumSize>();  //initialized to false
    is_Computed.set(ConeProperty::Generators);
    pointed = false;
    deg1_extreme_rays = false;
    deg1_generated = false;
    deg1_hilbert_basis = false;
    integrally_closed = false;
    
    reset_tasks();
    
    Extreme_Rays = vector<bool>(nr_gen,false);
    in_triang = vector<bool> (nr_gen,false);
    deg1_triangulation = true;
    if(dim==0){            //correction needed to include the 0 cone;
        multiplicity = 1;
        Hilbert_Series.add(vector<num_t>(1,1),vector<denom_t>());
        is_Computed.set(ConeProperty::HilbertSeries);
        is_Computed.set(ConeProperty::Triangulation);
    }
    pyr_level=-1;
    Top_Cone=this;
    Top_Key.resize(nr_gen);
    for(size_t i=0;i<nr_gen;i++)
        Top_Key[i]=i;
    totalNrSimplices=0;
    TriangulationSize=0;
    CandidatesSize=0;
    detSum = 0;
    
    FS.resize(omp_get_max_threads());
    
    Pyramids.resize(1);  // prepare storage for pyramids
    nrPyramids.resize(1);
    nrPyramids[0]=0;
      
    recursion_allowed=true;
    
    do_all_hyperplanes=true;
    // multithreaded_pyramid=true; now in build_cone where it is defined dynamically

    
    nextGen=0;
    store_level=0;
    
    Comparisons.reserve(nr_gen);
    nrTotalComparisons=0;

    inhomogeneous=false;
}

//---------------------------------------------------------------------------

template<typename Integer>
Full_Cone<Integer>::Full_Cone(const Cone_Dual_Mode<Integer> &C) {

    is_Computed = bitset<ConeProperty::EnumSize>();  //initialized to false

    dim = C.dim;
    Generators = C.get_generators();
    nr_gen = Generators.nr_of_rows();
    is_Computed.set(ConeProperty::Generators);
    Extreme_Rays=C.get_extreme_rays();
    is_Computed.set(ConeProperty::ExtremeRays);

    multiplicity = 0;
    in_triang = vector<bool>(nr_gen,false);
    
    pointed = true;
    is_Computed.set(ConeProperty::IsPointed);
    deg1_extreme_rays = false;
    deg1_generated = false;
    deg1_triangulation = false;
    deg1_hilbert_basis = false;
    integrally_closed = false;
    
    reset_tasks();
    
    for (size_t i=0; i < C.SupportHyperplanes.nr_of_rows(); i++) {
        Support_Hyperplanes.push_back(C.SupportHyperplanes[i]);
    }
    is_Computed.set(ConeProperty::SupportHyperplanes);
    
    if(!C.do_only_Deg1_Elements){
        Hilbert_Basis = C.Hilbert_Basis;
        is_Computed.set(ConeProperty::HilbertBasis);
    }
    else{
        Deg1_Elements =C.Hilbert_Basis;
        is_Computed.set(ConeProperty::Deg1Elements);
    }
    if(dim==0){            //correction needed to include the 0 cone;
        multiplicity = 1;
        Hilbert_Series.add(vector<num_t>(1,1),vector<denom_t>());
        is_Computed.set(ConeProperty::HilbertSeries);
    }
    pyr_level=-1;
    Top_Cone=this;
    Top_Key.resize(nr_gen);
    for(size_t i=0;i<nr_gen;i++)
        Top_Key[i]=i;
    totalNrSimplices=0;
    TriangulationSize=0;
    CandidatesSize=0;
    detSum = 0;
    
    do_all_hyperplanes=true;
    
    tri_recursion=false;
    
    nextGen=0;
    
    inhomogeneous=C.inhomogeneous;
}

template<typename Integer>
void Full_Cone<Integer>::dual_mode() {
    Support_Hyperplanes.sort();
    Support_Hyperplanes.unique();
    Support_Hyperplanes.remove(vector<Integer>(dim,0));

    if(dim>0 && !inhomogeneous) {            //correction needed to include the 0 cone;
        deg1_check();
        if (isComputed(ConeProperty::Grading) && !isComputed(ConeProperty::Deg1Elements)) {
            if (verbose) { 
                verboseOutput() << "Find degree 1 elements" << endl;
            }
            select_deg1_elements();
        }
    }
    if(dim==0){
        deg1_extreme_rays = deg1_generated = true;
        Grading=vector<Integer>(dim);
        is_Computed.set(ConeProperty::IsDeg1ExtremeRays);
        is_Computed.set(ConeProperty::IsDeg1Generated);
        is_Computed.set(ConeProperty::Grading);
    }
    if(!inhomogeneous && isComputed(ConeProperty::HilbertBasis)){
        if (isComputed(ConeProperty::Grading)) check_deg1_hilbert_basis();
        check_integrally_closed();
    }
    if(inhomogeneous){
       set_levels();
       find_module_rank();
       find_level0_dim();
       cout << "module rank " << module_rank << endl;
    }
}

//---------------------------------------------------------------------------

/* constructor for pyramids */
template<typename Integer>
Full_Cone<Integer>::Full_Cone(Full_Cone<Integer>& C, const vector<key_t>& Key) {

    Generators = C.Generators.submatrix(Key);
    dim = Generators.nr_of_columns();
    nr_gen = Generators.nr_of_rows();
    
    Top_Cone=C.Top_Cone; // relate to top cone
    Top_Key.resize(nr_gen);
    for(size_t i=0;i<nr_gen;i++)
        Top_Key[i]=C.Top_Key[Key[i]];
  
    multiplicity = 0;
    
    Extreme_Rays = vector<bool>(nr_gen,false);
    is_Computed.set(ConeProperty::ExtremeRays, C.isComputed(ConeProperty::ExtremeRays));
    if(isComputed(ConeProperty::ExtremeRays))
        for(size_t i=0;i<nr_gen;i++)
            Extreme_Rays[i]=C.Extreme_Rays[Key[i]];
    in_triang = vector<bool> (nr_gen,false);
    deg1_triangulation = true;
    
    Grading=C.Grading;
    is_Computed.set(ConeProperty::Grading, C.isComputed(ConeProperty::Grading));
    Order_Vector=C.Order_Vector;
    
    do_triangulation=C.do_triangulation;
    do_partial_triangulation=C.do_partial_triangulation;
    do_determinants=C.do_determinants;
    do_multiplicity=C.do_multiplicity;
    do_deg1_elements=C.do_deg1_elements;
    do_h_vector=C.do_h_vector;
    do_Hilbert_basis=C.do_Hilbert_basis;
    keep_triangulation=C.keep_triangulation;
    do_only_multiplicity=C.do_only_multiplicity;
    do_evaluation=C.do_evaluation;
    do_Stanley_dec=C.do_Stanley_dec;
    inhomogeneous=C.inhomogeneous;   // at present not used in proper pyramids
    is_pyramid=true;
    
    pyr_level=C.pyr_level+1;
    
    totalNrSimplices=0;
    detSum = 0;
    if(C.gen_degrees.size()>0){ // now we copy the degrees
    	gen_degrees.resize(nr_gen);
        for (size_t i=0; i<nr_gen; i++) {
            gen_degrees[i] = C.gen_degrees[Key[i]];
        }
    }
    if(C.gen_levels.size()>0){ // now we copy the levels
    	gen_levels.resize(nr_gen);
        for (size_t i=0; i<nr_gen; i++) {
            gen_levels[i] = C.gen_levels[Key[i]];
        }
    }
    TriangulationSize=0;
    CandidatesSize=0;
    
    recursion_allowed=C.recursion_allowed; // must be reset if necessary 
    do_all_hyperplanes=true; //  must be reset for non-recursive pyramids
    // multithreaded_pyramid=false; // SEE ABOVE
    
    nextGen=0;
    store_level=0;
    
    Comparisons.reserve(nr_gen);
    nrTotalComparisons=0;
}

//---------------------------------------------------------------------------

template<typename Integer>
bool Full_Cone<Integer>::isComputed(ConeProperty::Enum prop) const{
    return is_Computed.test(prop);
}

//---------------------------------------------------------------------------
// Data access
//---------------------------------------------------------------------------

template<typename Integer>
size_t Full_Cone<Integer>::getDimension()const{
    return dim;
}

//---------------------------------------------------------------------------

template<typename Integer>
size_t Full_Cone<Integer>::getNrGenerators()const{
    return nr_gen;
}

//---------------------------------------------------------------------------

template<typename Integer>
bool Full_Cone<Integer>::isPointed()const{
    return pointed;
}

//---------------------------------------------------------------------------

template<typename Integer>
bool Full_Cone<Integer>::isDeg1ExtremeRays() const{
    return deg1_extreme_rays;
}

template<typename Integer>
bool Full_Cone<Integer>::isDeg1HilbertBasis() const{
    return deg1_hilbert_basis;
}

template<typename Integer>
bool Full_Cone<Integer>::isIntegrallyClosed() const{
    return integrally_closed;
}

//---------------------------------------------------------------------------

template<typename Integer>
vector<Integer> Full_Cone<Integer>::getGrading() const{
    return Grading;
}

//---------------------------------------------------------------------------

template<typename Integer>
mpq_class Full_Cone<Integer>::getMultiplicity()const{
    return multiplicity;
}

//---------------------------------------------------------------------------

template<typename Integer>
Integer Full_Cone<Integer>::getShift()const{
    return shift;
}

//---------------------------------------------------------------------------

template<typename Integer>
size_t Full_Cone<Integer>::getModuleRank()const{
    return module_rank;
}


//---------------------------------------------------------------------------

template<typename Integer>
const Matrix<Integer>& Full_Cone<Integer>::getGenerators()const{
    return Generators;
}

//---------------------------------------------------------------------------

template<typename Integer>
vector<bool> Full_Cone<Integer>::getExtremeRays()const{
    return Extreme_Rays;
}

//---------------------------------------------------------------------------

template<typename Integer>
Matrix<Integer> Full_Cone<Integer>::getSupportHyperplanes()const{
    size_t s= Support_Hyperplanes.size();
    Matrix<Integer> M(s,dim);
    size_t i=0;
    typename list< vector<Integer> >::const_iterator l;
    for (l =Support_Hyperplanes.begin(); l != Support_Hyperplanes.end(); l++) {
        M.write(i,(*l));
        i++;
    }
    return M;
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::getTriangulation(list< vector<key_t> >& Triang, list<Integer>& TriangVol) const {
    Triang.clear();
    TriangVol.clear();
    vector<key_t> key(dim);
    typename list< SHORTSIMPLEX<Integer> >::const_iterator l;
    for (l =Triangulation.begin(); l != Triangulation.end(); l++) {
        key=l->key;
        Triang.push_back(key);
        TriangVol.push_back(l->height);
    }
}

//---------------------------------------------------------------------------

template<typename Integer>
Matrix<Integer> Full_Cone<Integer>::getHilbertBasis()const{
    size_t s= Hilbert_Basis.size();
    Matrix<Integer> M(s,dim);
    size_t i=0;
    typename list< vector<Integer> >::const_iterator l;
    for (l =Hilbert_Basis.begin(); l != Hilbert_Basis.end(); l++) {
        M.write(i,(*l));
        i++;
    }
    return M;
}

//---------------------------------------------------------------------------

template<typename Integer>
Matrix<Integer> Full_Cone<Integer>::getDeg1Elements()const{
    size_t s= Deg1_Elements.size();
    Matrix<Integer> M(s,dim);
    size_t i=0;
    typename list< vector<Integer> >::const_iterator l;
    for (l =Deg1_Elements.begin(); l != Deg1_Elements.end(); l++) {
        M.write(i,(*l));
        i++;
    }
    return M;
}

//---------------------------------------------------------------------------

template<typename Integer>
Matrix<Integer> Full_Cone<Integer>::getExcludedFaces()const{
    return(ExcludedFaces);
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::error_msg(string s) const{
    errorOutput() <<"\nFull Cone "<< s<<"\n";
}

//---------------------------------------------------------------------------

template<typename Integer>
void Full_Cone<Integer>::print()const{
    verboseOutput()<<"\ndim="<<dim<<".\n";
    verboseOutput()<<"\nnr_gen="<<nr_gen<<".\n";
    // verboseOutput()<<"\nhyp_size="<<hyp_size<<".\n";
    verboseOutput()<<"\nGrading is:\n";
    verboseOutput()<< Grading;
    verboseOutput()<<"\nMultiplicity is "<<multiplicity<<".\n";
    verboseOutput()<<"\nGenerators are:\n";
    Generators.read();
    verboseOutput()<<"\nExtreme_rays are:\n";
    verboseOutput()<< Extreme_Rays;
    verboseOutput()<<"\nSupport Hyperplanes are:\n";
    verboseOutput()<< Support_Hyperplanes;
    verboseOutput()<<"\nTriangulation is:\n";
    verboseOutput()<< Triangulation;
    verboseOutput()<<"\nHilbert basis is:\n";
    verboseOutput()<< Hilbert_Basis;
    verboseOutput()<<"\nDeg1 elements are:\n";
    verboseOutput()<< Deg1_Elements;
    verboseOutput()<<"\nHilbert Series  is:\n";
    verboseOutput()<<Hilbert_Series;
}

} //end namespace


