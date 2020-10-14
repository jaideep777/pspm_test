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

//template<class Model>
//const int Solver<Model>::xsize(){
//    if (method == SOLVER_FMU) return J;	
//    if (method == SOLVER_MMU) return J;  
//    if (method == SOLVER_CM ) return J;
//    if (method == SOLVER_EBT) return J;
//}

template<class Model>
Solver<Model>::Solver(PSPM_SolverType _method) : odeStepper(0, 1e-6, 1e-6) {
	method = _method;
}


template<class Model>
int Solver<Model>::setupLayout(Species<Model> &s){
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
	s.clearVars();
	if (method == SOLVER_FMU){ 
		s.addVar("u", 1, s.J);		// uuuuu..
	}
	else{
		s.addVar("X", 1, s.J);		// xxxxx.. uuuuu..
		s.addVar("u", 1, s.J);		// 
	}
	for (int i=0; i < s.varnames_extra.size(); ++i){
		s.addVar(s.varnames_extra[i], s.varnames_extra.size(), 1);  // abc abc abc .. 
	}

	return s.size();
}


template<class Model>
void Solver<Model>::addSpecies(std::vector<double> xbreaks, Model* _mod, 
							   std::vector<std::string> extra_vars, double input_birth_flux){
	Species<Model> s;
	s.mod = _mod;
	s.varnames_extra = extra_vars;
	s.set_inputBirthFlux(input_birth_flux);
	
	s.xb = xbreaks[0];
	s.xm = xbreaks[xbreaks.size()-1];

	if (method == SOLVER_FMU) s.J = xbreaks.size()-1;	
	if (method == SOLVER_MMU) s.J = xbreaks.size()-1;  
	if (method == SOLVER_CM ) s.J = xbreaks.size();
	if (method == SOLVER_EBT) s.J = xbreaks.size();

	s.x = xbreaks;

	int state_size = setupLayout(s);

	s.start_index = state.size();	// New species will be appended to the end of state vector
	state.resize(state.size()+state_size);  // This will resize state for all species additions, but this in only initialization so its okay.
	rates.resize(state.size()+state_size);

	species_vec.push_back(s);
}


template<class Model>
void Solver<Model>::addSpecies(int _J, double _xb, double _xm, bool log_breaks, Model* _mod,
							   std::vector<std::string> extra_vars, double input_birth_flux){
	vector<double> xbreaks;
	if (log_breaks) xbreaks = logseq(_xb, _xm, _J+1);
	else            xbreaks =    seq(_xb, _xm, _J+1);

	addSpecies(xbreaks, _mod, extra_vars, input_birth_flux);
}


template<class Model>
void Solver<Model>::resetState(){
   	current_time = 0;
	odeStepper = RKCK45<vector<double>> (0, control.ode_eps, control.ode_initial_step_size);  // this is a cheap operation, but this will empty the internal containers, which will then be (automatically) resized at next 1st ODE step. Maybe add a reset function to the ODE stepper? 

	// state.resize(state_size);   // state will be resized by addSpecies
	//rates.resize(state.size());
	std::fill(state.begin(), state.end(), 0); 
	std::fill(rates.begin(), rates.end(), -999); // DEBUG

	// initialize grid/cohorts for each species
	for (int k=0; k<species_vec.size(); ++k){
		Species<Model> &s = species_vec[k]; // easy reference 

		if (method == SOLVER_FMU){	
			s.X.resize(s.J);
			for (size_t i=0; i<s.J; ++i) s.X[i] = (s.x[i]+s.x[i+1])/2.0;
			
			s.h.resize(s.J);	// This will be used only by FMU
			for (size_t i=0; i<s.J; ++i) s.h[i] = s.x[i+1] - s.x[i];	

			// no X in FMU state
		}

		if (method == SOLVER_MMU){
			for (size_t i=0; i<s.J; ++i) state[s.start_index + i] = s.x[i];  // skip x_J as it is fixed. 
		}

		if (method == SOLVER_CM){
			for (size_t i=0; i<s.J; ++i) state[s.start_index + i] = s.x[i];
		}

		if (method == SOLVER_EBT){
			for (size_t i=1; i<s.J; ++i) state[s.start_index + i] = (s.x[i]+s.x[i-1])/2.0; // leave [0] for pi0 (= 0)
		}
	}
	//u0_out_history.clear();
}




template<class Model>
void Solver<Model>::print(){
	std::cout << ">> SOLVER \n";
	string types[] = {"FMU", "MMU", "CM", "EBT"};
	std::cout << "+ Type: " << types[method] << std::endl;

	std::cout << "+ State size = " << state.size() << "\n";
	std::cout << "+ Species:\n";
	for (int i=0; i<species_vec.size(); ++i) {
		std::cout << "Sp (" << i << "):\n";
		species_vec[i].print(state, rates);
	}
	//IteratorSet<vector<double>::iterator> iset(state.begin(), varnames.size(), vars, xsize());
	
}


template <class Model>
void Solver<Model>::initialize(){

	for (int k=0; k<species_vec.size(); ++k){
		Species<Model> &s = species_vec[k];

		if (method == SOLVER_FMU){
			for (size_t i=0; i<s.J; ++i)  state[s.start_index + i] = s.mod->initDensity((s.x[i]+s.x[i+1])/2);
		}
		if (method == SOLVER_CM){
			for (size_t i=0; i<s.J; ++i)  state[s.start_index + s.J + i] = log(s.mod->initDensity(s.x[i]));
		}
		if (method == SOLVER_EBT){
			for (size_t i=1; i<s.J; ++i)  state[s.start_index + s.J + i] = s.mod->initDensity((s.x[i]+s.x[i-1])/2)*(s.x[i]-s.x[i-1]);	// state[J+1+0]=0 (N0)
		}

		if (s.varnames_extra.size() > 0){  // If extra state variables have been requested, initialize them
			// Initialize extra size-dependent variables
			auto is = s.get_iterators(state);
			
			auto& itx = is.get("X");
			int id = is.getIndex(s.varnames_extra[0]); // get index of 1st extra variable
			for (is.begin(); !is.end(); ++is){
				vector <double> v = s.mod->initStateExtra(*itx, current_time);  // returned vector will be `move`d so this is fast 
				auto it_vec = is.get();
				for (size_t i = 0; i<s.varnames_extra.size(); ++i){
					*it_vec[id+i] = v[i];
				}
			}
		}
	}
}

/*
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


*/


