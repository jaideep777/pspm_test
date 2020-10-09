//#include "solver.h"

#include <iostream>
#include <cmath>
#include <cassert>
#include <string>

#include "iterator_set.h"

using namespace std;


std::vector <double> seq(double from, double to, int len){
	std::vector<double> x(len);
	for (size_t i=0; i<len; ++i) x[i] = from + i*(to-from)/(len-1);
	return x;
}

std::vector <double> logseq(double from, double to, int len){
	std::vector<double> x(len);
	for (size_t i=0; i<len; ++i) x[i] = exp(log(from) + i*(log(to)-log(from))/(len-1));
	return x;
}
// ~~~~~~~~~~~ SOLVER ~~~~~~~~~~~~~~~~~~~~~

template<class Model>
const int Solver<Model>::xsize(){
	if (method == SOLVER_FMU) return J;	
	if (method == SOLVER_MMU) return J;  
	if (method == SOLVER_CM ) return J;
	if (method == SOLVER_EBT) return J;
}


template<class Model>
void Solver<Model>::setupLayout(){
	// Set up layout of state vector, for e.g.
	//  ------------------------------------------------------------
	// | x | x | x : u | u | u : a | b | c | a | b | c | a | b | c |
	//  ------------------------------------------------------------
	//  In above layout, the internal variables x and u are tightly
	//  packed, and the extra variables a, b, c are interleaved. 
	//  This arrangement is cache friendly because:
	//  1. When calculating integrals, x and u are traversed
	//  2. When setting rates, typically a,b,c are calculated one 
	//  after the other for each x.
	auto addVar = [this](std::string name, int stride, int offset){
		varnames.push_back(name);
		strides.push_back(stride);
		offsets.push_back(offset);
	};
	varnames.clear(); strides.clear(); offsets.clear(); 
	if (method == SOLVER_FMU){ 
		addVar("u", 1, xsize());		// uuuuu..
	}
	else{
		addVar("X", 1, xsize());		// xxxxx.. uuuuu..
		addVar("u", 1, xsize());		// 
	}
	for (int i=0; i<varnames_extra.size(); ++i){
		addVar(varnames_extra[i], varnames_extra.size(), 1);  // abc abc abc .. 
	}
}


template<class Model>
void Solver<Model>::resetState(const std::vector<double>& xbreaks){
   	current_time = 0;
	odeStepper = RKCK45<vector<double>> (0, control.ode_eps, control.ode_initial_step_size);  // this is a cheap operation, but this will empty the internal containers, which will then be (automatically) resized at next 1st ODE step. Maybe add a reset function to the ODE stepper? 

	xb = xbreaks[0];
	xm = xbreaks[xbreaks.size()-1];

	if (method == SOLVER_FMU) J = xbreaks.size()-1;	
	if (method == SOLVER_MMU) J = xbreaks.size()-1;  
	if (method == SOLVER_CM ) J = xbreaks.size();
	if (method == SOLVER_EBT) J = xbreaks.size();

	x = xbreaks;

	setupLayout();

	state.resize(varnames.size()*xsize());   // xsize() is size of X/U vectors in state, = J
	rates.resize(state.size());
	std::fill(state.begin(), state.end(), 0); 
	std::fill(rates.begin(), rates.end(), -999); // DEBUG

	// initialize X
	if (method == SOLVER_FMU){	
		X.resize(J);
		for (size_t i=0; i<J; ++i) X[i] = (xbreaks[i]+xbreaks[i+1])/2.0;
		
		h.resize(J);	// This will be used only by FMU
		for (size_t i=0; i<J; ++i) h[i] = xbreaks[i+1] - xbreaks[i];	

		// no X in FMU state
	}

	if (method == SOLVER_MMU){
		for (size_t i=0; i<J; ++i) state[i] = xbreaks[i];  // skip x_J as it is fixed. 
	}

	if (method == SOLVER_CM){
		for (size_t i=0; i<J; ++i) state[i] = xbreaks[i];
	}

	if (method == SOLVER_EBT){
		for (size_t i=1; i<J; ++i) state[i] = (xbreaks[i]+xbreaks[i-1])/2.0; // leave [0] for pi0 (= 0)
	}
   
	u0_out_history.clear();
}

template<class Model>
Solver<Model>::Solver(std::vector<double> xbreaks, PSPM_SolverType _method) : odeStepper(0, 1e-6, 1e-6) {
	method = _method;
	resetState(xbreaks);	
}

template<class Model>
Solver<Model>::Solver(int _J, double _xb, double _xm, PSPM_SolverType _method) 
	: Solver(seq(_xb, _xm, _J+1), _method){
}


template<class Model>
void Solver<Model>::setModel(Model *M){
	mod = M;
}


template<class Model>
void Solver<Model>::setInputNewbornDensity(double input_u0){
	u0_in = input_u0;
}


template<class Model>
const int Solver<Model>::size(){
	return state.size();
}


template<class Model>
double Solver<Model>::getMaxSize(vector<double>::iterator sbegin){
	if (method == SOLVER_FMU) return *x.rbegin();
	else {
		return *next(sbegin, xsize()-1);
	}
}


	//for (int i=0; i<J; ++i){
	//    x[i+1] = exp(log(0.01) + (i)*(log(xm)-log(0.01))/(J-1));
	//    h[i] = x[i+1]-x[i];
	//    X[i] = (x[i+1]+x[i])/2;
	//}



template<class Model>
const double * Solver<Model>::getX(){
	auto iset = getIterators_state();
	return &(*iset.get("X"));
}

template<class Model>
vector<double> Solver<Model>::getx(){
	return x;
}


template<class Model>
IteratorSet<vector<double>::iterator> Solver<Model>::getIterators_state(){
	
	IteratorSet<vector<double>::iterator> iset(state.begin(), varnames, xsize(), offsets, strides);
	if (method == SOLVER_FMU) iset.push_back("X", X.begin(), 1);
	return iset;
}


template<class Model>
IteratorSet<vector<double>::iterator> Solver<Model>::getIterators_rates(){
	return IteratorSet<vector<double>::iterator> (rates.begin(), varnames, xsize(), offsets, strides);
}


template<class Model>
IteratorSet<vector<double>::iterator> Solver<Model>::createIterators_state(vector<double> &v){
	IteratorSet<vector<double>::iterator> iset(v.begin(), varnames, xsize(), offsets, strides);
	if (method == SOLVER_FMU) iset.push_back("X", X.begin(), 1);
	return iset;
}

template<class Model>
IteratorSet<vector<double>::iterator> Solver<Model>::createIterators_rates(vector<double> &v){
	IteratorSet<vector<double>::iterator> iset(v.begin(), varnames, xsize(), offsets, strides);
	return iset;
}


template<class Model>
void Solver<Model>::print(){
	string types[] = {"FMU", "MMU", "CM", "EBT"};
	std::cout << "Type: " << types[method] << std::endl;

	//IteratorSet<vector<double>::iterator> iset(state.begin(), varnames.size(), vars, xsize());
	auto iset = getIterators_state();

	if (method == SOLVER_FMU){
		iset.push_back("_X", X.begin(),1);
		iset.push_back("_h", h.begin(),1);
		std::cout << "x (" << x.size() << "): "; 
		for (auto xx : x) std::cout << xx << " ";
		std::cout << "\n";
	}

	std::cout << "State (" << state.size() << "):\n";
	iset.print();
	
	std::cout << "Rates (" << rates.size() << "):\n";
	auto irates = getIterators_rates();
	irates.print();
	
}





template <class Model>
void Solver<Model>::initialize(){
	// state vector was initialized to 0 in Constrctor. Set non-zero elements here
	//vector<double> X0(J), h(J);
	//for (int i=0; i<X0.size(); ++i) X0[i] = (x[i]+x[i+1])/2;
	//for (int i=0; i<h.size(); ++i) h[i] = x[i+1]-x[i];
	

	if (method == SOLVER_FMU){
		for (size_t i=0; i<J; ++i)  state[i] = mod->initDensity((x[i]+x[i+1])/2);
	}
	if (method == SOLVER_MMU){
		for (size_t i=0; i<J; ++i)  state[J + i] = mod->initDensity((x[i]+x[i+1])/2);
		//for (size_t i=0; i<J+1; ++i) uprev[i] = initDensity(x[i]);
	}
	if (method == SOLVER_CM){
		for (size_t i=0; i<J; ++i)  state[J + i] = log(mod->initDensity(x[i]));
	}
	if (method == SOLVER_EBT){
		for (size_t i=1; i<J; ++i)  state[J + i] = mod->initDensity((x[i]+x[i-1])/2)*(x[i]-x[i-1]);	// state[J+1+0]=0 (N0)
	}

	if (varnames_extra.size() > 0){  // If extra state variables have been requested, initialize them
		// Initialize extra size-dependent variables
		auto is = getIterators_state();
		
		auto& itx = is.get("X");
		int id = is.getIndex(varnames_extra[0]); // get index of 1st extra variable
		for (is.begin(); !is.end(); ++is){
			vector <double> v = mod->initStateExtra(*itx, current_time);  // returned vector will be `move`d so this is fast 
			auto it_vec = is.get();
			for (size_t i = 0; i<varnames_extra.size(); ++i){
				*it_vec[id+i] = v[i];
			}
		}
	}
}


template<class Model>
void Solver<Model>::calcRates_extra(double t, vector<double>&S, vector<double>& dSdt){
	//auto is = createIterators_state(S);
	//auto ir = createIterators_rates(dSdt);
	//auto& itx = is.get("X");
	//auto& itre = ir.get(varnames_extra[0]);
	
	//for (is.begin(), ir.begin(); !is.end(); ++is, ++ir){
	//    auto it_returned = mod->calcRates_extra(t, *itx, itre);
	//    assert(distance(itre, it_returned) == varnames_extra.size());
	//}
}

// current_time is updated by the ODE solver at every (internal) step
template<class Model>
void Solver<Model>::step_to(double tstop){
	// do nothing if tstop is <= current_time
	if (tstop <= current_time) return;
	
	if (method == SOLVER_FMU){	
		auto derivs = [this](double t, vector<double> &S, vector<double> &dSdt){
			mod->computeEnv(t, S, this);
			this->calcRates_FMU(t, S, dSdt);
			if (varnames_extra.size() > 0) this->calcRates_extra(t, S, dSdt);
		};
		
		odeStepper.Step_to(tstop, current_time, state, derivs); // state = [U]
	}
	if (method == SOLVER_MMU){
	}
	if (method == SOLVER_EBT){
		auto derivs = [this](double t, vector<double> &S, vector<double> &dSdt){
			mod->computeEnv(t, S, this);
			this->calcRates_EBT(t, S, dSdt);
			if (varnames_extra.size() > 0) this->calcRates_extra(t, S, dSdt);
		};
		
		// integrate 
		odeStepper.Step_to(tstop, current_time, state, derivs); // state = [pi0, Xint, N0, Nint]
		
		// update cohorts
		removeDeadCohorts_EBT();
		if (state[xsize()] > 0) addCohort_EBT();  // Add new cohort if N0 > 0. Add after removing dead ones otherwise this will also be removed. 
	}
	if (method == SOLVER_CM){
		auto derivs = [this](double t, vector<double> &S, vector<double> &dSdt){
			mod->computeEnv(t, S, this);
			this->calcRates_CM(t, S, dSdt);
			//if (varnames_extra.size() > 0) this->calcRates_extra(t, S, dSdt);
		};
		
		// integrate 
		odeStepper.Step_to(tstop, current_time, state, derivs); // state = [pi0, Xint, N0, Nint]

		//// update cohorts
		addCohort_CM();		// add before so that it becomes boundary cohort and first internal cohort can be (potentially) removed
		//removeCohort_CM();
		mod->computeEnv(current_time, state, this); // is required here IF rescaleEnv is used in derivs
	}
}


template<class Model>
double Solver<Model>::newborns_out(){
	// update Environment from latest state
	mod->computeEnv(current_time, state, this);
	// calculate birthflux 
	double birthFlux = integrate_x([this](double z, double t){return mod->birthRate(z,t);}, current_time, state, 1);
	return birthFlux;
}


template<class Model>
double Solver<Model>::u0_out(){
	return newborns_out()/mod->growthRate(xb, current_time);
}

template<class Model>
double Solver<Model>::get_u0_out(){
	return u0_out_history.back();
}


template<class Model>
double Solver<Model>::stepToEquilibrium(){
	for (double t=0.05; ; t=t+0.05) {
		step_to(t);
		
		u0_out_history.push_back(u0_out());
		if (u0_out_history.size() > 5) u0_out_history.pop_front();
		//cout << t << " | ";  
		//for (auto u : u0_out_history) cout << u << " "; // cout << "\n";
		double max_err = -1e20;
		//cout << u0_out_history.size() << endl; 
		for (auto it = ++u0_out_history.begin(); it != u0_out_history.end(); ++it){
			double err = *it - *(prev(it)); 
			max_err = std::max(max_err, abs(err));
			//cout << err << " ";
		}
		//cout << " | " << max_err << endl;
		
		if (abs(max_err) < control.convergence_eps){
			cout << t << " | " << J << " | "; for (auto u : u0_out_history) cout << u << " "; cout << "|" << max_err << endl;
			break;
		}	
	}
	
}



template<class Model>
double Solver<Model>::createSizeStructuredVariables(vector<std::string> names){
	varnames_extra = names;
	resetState(x);
}


#include "mu.tpp"
#include "ebt.tpp"
#include "cm.tpp"
#include "size_integrals.tpp"




