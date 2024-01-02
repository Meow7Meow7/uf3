#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <H5Cpp.h>
#include <string>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>

#include "UltraFastFeaturize.h"

#include "uf3_bspline_basis3.h"

namespace py = pybind11;

UltraFastFeaturize::UltraFastFeaturize(int _degree,
                             int _nelements,
                             py::tuple _interactions_map,
                             py::array_t<double, py::array::c_style> _n2b_knots_map,
                             py::array_t<int, py::array::c_style> _n2b_num_knots)
                             //py::array_t<double, py::array::c_style> _data)
        : degree(_degree), nelements(_nelements), 
          interactions_map(_interactions_map),
          n2b_knots_map(_n2b_knots_map),
          n2b_num_knots(_n2b_num_knots),
          //data(_data),
          // As bspline_config_ff conatins const members, we have to use the
          // copy constructor of 'bspline_config_ff' and initialize the 
          // BsplineConfig here
          BsplineConfig(degree, nelements, interactions_map, n2b_knots_map, n2b_num_knots)
{
  num_of_interxns = {static_cast<int>(this->BsplineConfig.n2b_interactions)};
  n2b_types = this->BsplineConfig.n2b_types;
  elements = std::vector<int> (nelements,0);
  for (int i=0; i<nelements; i++)
    elements[i] = interactions_map[i].cast<int>();

  rmin_max_2b_sq = this->BsplineConfig.rmin_max_2b_sq;

  ////Create bspline basis constant
  auto n2b_knots_map_un = n2b_knots_map.unchecked<2>();
  auto n2b_num_knots_un = n2b_num_knots.unchecked<1>();
  //Find max_num_2b_knots
  int max_num_2b_knots = 0;
  for (int interxn=0; interxn < num_of_interxns[0]; interxn++)
    max_num_2b_knots = std::max(max_num_2b_knots, n2b_num_knots_un(interxn));

  //Calculate constants in 2b bsline basis set
  //Create constants array
  constants_2b.resize(num_of_interxns[0]);
  for (int interxn=0; interxn < num_of_interxns[0]; interxn++){
    constants_2b[interxn].resize(max_num_2b_knots);
    for (int knot_no = 0; knot_no < max_num_2b_knots; knot_no++)
      constants_2b[interxn][knot_no].resize(16,0);
  }

  n2b_num_knots_array = std::vector<int> (num_of_interxns[0], 0);
  n2b_knots_array = std::vector<std::vector<double>> (num_of_interxns[0], std::vector<double>(max_num_2b_knots));

  reprn_length = 1+nelements;
  for (int interxn=0; interxn < num_of_interxns[0]; interxn++) {
    //clamped knots --> -4
    for (int knot_no = 0; knot_no < n2b_num_knots_un(interxn) - 4; knot_no++) {
      double *temp_knots = new double[5] {n2b_knots_map_un(interxn,knot_no),
                                          n2b_knots_map_un(interxn,knot_no+1),
                                          n2b_knots_map_un(interxn,knot_no+2),
                                          n2b_knots_map_un(interxn,knot_no+3),
                                          n2b_knots_map_un(interxn,knot_no+4)};

      std::vector<double> c = get_constants(temp_knots);

      for (int i=0; i < 16; i++)
        constants_2b[interxn][knot_no][i] = (std::isinf(c[i]) || 
                                             std::isnan(c[i])) ? 0 : c[i];
      delete[] temp_knots;
    }

    n2b_num_knots_array[interxn] = n2b_num_knots_un(interxn);
    for (int knot_no = 0; knot_no < max_num_2b_knots; knot_no++)
      n2b_knots_array[interxn][knot_no] = n2b_knots_map_un(interxn,knot_no);

    reprn_length = reprn_length + n2b_num_knots_un(interxn)-4;
  }
}

UltraFastFeaturize::~UltraFastFeaturize()
{}
//TODO: In the current implementation the data coming from python is getting copied
//Instead we can use py::buffer, this will allow access to the data without copying
/*Eg. void process_array(py::buffer b) {
    py::buffer_info info = b.request(); // Request buffer info

    // Check if the buffer is indeed a NumPy array or compatible
    if (info.format != py::format_descriptor<float>::format())
        throw std::runtime_error("Incompatible format; expected a float array.");

    // Access the buffer's data pointer directly
    float *data = static_cast<float *>(info.ptr);
    size_t N = info.size;

    // Example processing: scale each element in the array
    for (size_t i = 0; i < N; i++) {
        data[i] *= 2.0;
    }
}*/
//One of the requirement of the above is that the numpy array in python is Contiguous
//ie arr_contiguous = np.ascontiguousarray(arr)
void UltraFastFeaturize::set_geom_data(py::array_t<double, py::array::c_style> _atoms_array,
                                  py::array_t<double, py::array::c_style> _cell_array,
                                  py::array_t<int, py::array::c_style> _crystal_index,
                                  py::array_t<int, py::array::c_style> _supercell_factors,
                                  py::array_t<int, py::array::c_style> _geom_posn)
                                  //py::array_t<double, py::array::c_style> _supercell_array)
{
  atoms_array = _atoms_array;
  cell_array = _cell_array;
  crystal_index = _crystal_index;
  supercell_factors = _supercell_factors;
  geom_posn = _geom_posn;
  auto geom_posn_un = geom_posn.unchecked<1>();

  //atoms_array.shape[0] == crystal_index.shape[0]
  if (atoms_array.shape(0) != crystal_index.shape(0))
    throw std::length_error("atoms_array.shape[0] != crystal_index.shape[0]");

  //Check if len(atoms_array) == geom_posn[-1]
  if (atoms_array.shape(0) != geom_posn_un(geom_posn.shape(0)-1))
    throw std::length_error("atoms_array.shape[0] != geom_posn[geom_posn.shape[0]-1]");
  
  //cell_array.shape[0] == supercell_factors.shape[0] == geom_posn.shape[0]-1
  if (cell_array.shape(0) != supercell_factors.shape(0) && 
          supercell_factors.shape(0) != geom_posn.shape(0)-1)
    throw std::length_error("cell_array.shape[0] != supercell_factors.shape[0] != geom_posn.shape[0]-1");

  //auto supercell_factors_un = supercell_factors.unchecked<2>();

  //Loop over all crystal structures and find the max num of neigh an atom
  //can have
  /*max_num_neigh = 0;
  for (int i=0; i<= cell_array.shape(0); i++){
    int supercell_size = supercell_factors_un(i,0)*supercell_factors_un(i,1)*supercell_factors_un(i,2);
    supercell_size = supercell_size*(geom_posn_un(i+1)-geom_posn_un(i));
    max_num_neigh = std::max(max_num_neigh,supercell_size);
  }*/

}

py::array UltraFastFeaturize::featurize(int _batch_size, bool return_Neigh,
                                    std::string& _filename)
{
  //Create Neighs to store distance of neighbors by interaction
  //Remember Neighs has to be contigous as we are returning a 3d numpy array
  //in std::vectors only the first dimension is garunteed to be contigous

  //set batch_size
  batch_size = _batch_size;
  //change batch_size if needed
  if (static_cast<int>(atoms_array.shape(0)) < batch_size)
    batch_size = static_cast<int>(atoms_array.shape(0));

  //Handle filename
  filename = _filename;
  std::ifstream file(filename);
  if (file.good()){
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);

    // Format time
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_c), "%Y_%m_%d_%H_%M_%S");
    std::string datetime = ss.str();
    // Append formatted time to filename
    filename = filename + "_" + datetime + ".h5";
  }
  //Create HDF5 file
  H5::H5File feature_file(filename, H5F_ACC_TRUNC);
  write_hdf5_counter=0;

  //Get unchecked proxy object for fast access
  auto atoms_array_un = atoms_array.unchecked<2>();
  auto crystal_index_un = crystal_index.unchecked<1>();
  auto cell_array_un = cell_array.unchecked<3>();
  auto geom_posn_un = geom_posn.unchecked<1>();
  auto supercell_factors_un = supercell_factors.unchecked<2>();

  ///////Create Neigh list

  // Total number of batches
  num_batches =  static_cast<int>(std::ceil(static_cast<double>(atoms_array_un.shape(0))/batch_size));

  //Declare dimesnions of Neigh
  int depth, rows, cols;

  // Loop over batches
  for (int batch_numb =0; batch_numb<num_batches; batch_numb++) {

    //Get batch_start, batch_end; these indices 
    //are for atoms_array
    int batch_start = batch_numb*batch_size;
    int batch_end = batch_start+batch_size;
    if (batch_end>=atoms_array_un.shape(0)) {
      batch_end = static_cast<int>(atoms_array_un.shape(0));
      batch_size = batch_end-batch_start;
    }

    //Decide the depth of Neighs
    depth = batch_size;

    //Decide rows in Neigh
    //rows = static_cast<int>(this->BsplineConfig.n2b_interactions);
    rows = num_of_interxns[0];

    //Decide max_num_neigh in this batch
    //Loop over all crystal structures in this batch and find the max num of 
    //neigh an atom can have
    max_num_neigh = 0;

    //get crystal index of all the atoms in this batch;  using the crystal index 
    //get the supercell_factors of all the atoms in this batch
    for (int i=crystal_index_un(batch_start); i<=crystal_index_un(batch_end-1); i++){
      int supercell_size = supercell_factors_un(i,0)*supercell_factors_un(i,1)*supercell_factors_un(i,2);
      supercell_size = supercell_size*(geom_posn_un(i+1)-geom_posn_un(i));
      max_num_neigh = std::max(max_num_neigh,supercell_size);
    }
    cols = static_cast<int>(max_num_neigh);

    ////----------------Find Neighs-------------------////
    //If Neighs has never been initalized
    if (Neighs.empty()) {
      Neighs = std::vector<double>(depth*rows*cols,0);
      Tot_num_Neighs = std::vector<double>(depth*rows,0);
    }
    
    //Clear and shrink any existing Neighs
    else {
      Neighs.clear();
      Neighs.shrink_to_fit();
      Neighs.resize(depth*rows*cols);
      std::fill(Neighs.begin(), Neighs.end(), 0);

      Tot_num_Neighs.clear();
      Tot_num_Neighs.shrink_to_fit();
      Tot_num_Neighs.resize(depth*rows,0);
      std::fill(Tot_num_Neighs.begin(), Tot_num_Neighs.end(), 0);
    }

    //Loop over central atoms in this batch
    for (int atom1=batch_start; atom1<batch_end; atom1++) {
    //for (int atom1=0; atom1<batch_size; atom1++) {

      //get the atomic number; x,y,z cart-cordinates; crystal_index; lattice_vects
      //aka cells; geom_posn and supercell_factors
      int Z1 = atoms_array_un(atom1,0); 
      double x1 = atoms_array_un(atom1,1);
      double y1 = atoms_array_un(atom1,2);
      double z1 = atoms_array_un(atom1,3);
      int CI1 = crystal_index_un(atom1);
      double (*cell)[3] = new double[3][3] {
          {cell_array_un(CI1,0,0), cell_array_un(CI1,0,1), cell_array_un(CI1,0,2)},
          {cell_array_un(CI1,1,0), cell_array_un(CI1,1,1), cell_array_un(CI1,1,2)},
          {cell_array_un(CI1,2,0), cell_array_un(CI1,2,1), cell_array_un(CI1,2,2)}
      };

      double latx = sqrt((cell[0][0]*cell[0][0]) + (cell[1][0]*cell[1][0]) + 
              (cell[2][0]*cell[2][0])); //length of x lattice vector
      double laty = sqrt((cell[0][1]*cell[0][1]) + (cell[1][1]*cell[1][1]) + 
              (cell[2][1]*cell[2][1])); //length of y lattice vector
      double latz = sqrt((cell[0][2]*cell[0][2]) + (cell[1][2]*cell[1][2]) + 
              (cell[2][2]*cell[2][2])); //length of z lattice vector

      //Start and end index of atoms contained in the crystal to which this atom 
      //belongs to. This index is the index in atoms_array
      int start_posn = geom_posn_un(CI1);
      int end_posn = geom_posn_un(CI1+1);
      int num_atoms = end_posn-start_posn;
    
      int scf1 = supercell_factors_un(CI1, 0); //supercell size along x
      int scf2 = supercell_factors_un(CI1, 1); //y
      int scf3 = supercell_factors_un(CI1, 2); //z

      int n2b_interactions = num_of_interxns[0]; //total 2-body interactions
      std::vector<int> posn_to_neigh(n2b_interactions,0); //holds index of Neigh 
                                                          //for each interaction
                                                          //at which the neigh is inserted
                                                          //Initialize to 0
    
      int d = atom1-batch_start; //d=0 is the first atom in this batch

      //Loop over all atoms of the crystal to which this atom belongs to
      for (int atom2=start_posn; atom2<end_posn; atom2++) {
        //if (atom2 == atom1) continue;
        int Z2 = atoms_array_un(atom2,0); 
        double x2 = atoms_array_un(atom2,1);
        double y2 = atoms_array_un(atom2,2);
        double z2 = atoms_array_un(atom2,3);
        if (crystal_index_un(atom2)!=CI1)
          throw std::domain_error("atom1 and atom2 belong to different crystals");
      
        //Determine the interaction type
        int n2b_type = -1;
        for (int i=0; i<n2b_interactions; i++) {
          if ((Z1==n2b_types[2*i]) && (Z2==n2b_types[2*i+1])) n2b_type = i;
          if ((Z2==n2b_types[2*i]) && (Z1==n2b_types[2*i+1])) n2b_type = i;      
        }

        double rmin_sq = rmin_max_2b_sq[2*n2b_type];
        double rmax_sq = rmin_max_2b_sq[2*n2b_type+1];

        //Loop over all periodic images of atom2

        //int d = atom1-batch_start; //d=0 is the first atom in this batch
        int r = n2b_type;
        int c = 0;// = posn_to_neigh[n2b_type];

        // atom in (0,0,0) supercell aka the unit cell
        /*double rsq = pow((x2-x1),2) + pow((y2-y1),2) + pow((z2-z1),2);
        if ((rmin_sq <= rsq) && (rsq < rmax_sq)){
          Neighs[d*(rows*cols)+(r*cols)+c] = sqrt(rsq);
          posn_to_neigh[n2b_type]++;
        }*/

        for (int ix=-1*scf1; ix<=scf1; ix++) {
        //for (int ix=0; ix<=scf1; ix++) {
          double offset_x[3];
          offset_x[0] = ix*cell[0][0];
          offset_x[1] = ix*cell[0][1];
          offset_x[2] = ix*cell[0][2];

          for (int iy=-1*scf2; iy<=scf2; iy++) {
          //for (int iy=0; iy<=scf2; iy++) {
            double offset_y[3];
            offset_y[0] = iy*cell[1][0];
            offset_y[1] = iy*cell[1][1];
            offset_y[2] = iy*cell[1][2];

            for (int iz=-1*scf3; iz<=scf3; iz++) {
            //for (int iz=0; iz<=scf3; iz++) {
              double offset_z[3];
              offset_z[0] = iz*cell[2][0];
              offset_z[1] = iz*cell[2][1];
              offset_z[2] = iz*cell[2][2];
              
              //x2_pi, y2_pi, z2_pi -> cordinate of periodic image of atom2  
              double x2_pi = offset_x[0] + offset_y[0] + offset_z[0] + x2;
              //double x2_pi_neg = (-2*ix*latx) + x2_pi;
              
              double y2_pi = offset_x[1] + offset_y[1] + offset_z[1] + y2;
              //double y2_pi_neg = (-2*iy*laty) + y2_pi;
              
              double z2_pi = offset_x[2] + offset_y[2] + offset_z[2] + z2;
              //double z2_pi_neg = (-2*iz*latz) + z2_pi;
              
              //+x,+y,+z
              double rsq = pow((x2_pi-x1),2) + pow((y2_pi-y1),2) + pow((z2_pi-z1),2);
              if ((rmin_sq <= rsq) && (rsq < rmax_sq)){
                int temp_index = d*(rows*cols)+(r*cols)+posn_to_neigh[n2b_type];
                Neighs[temp_index] = sqrt(rsq);
                posn_to_neigh[n2b_type]++;
              }
            }
          }
        }
      } //End of atom2 loop
      for (int i=0; i<n2b_interactions; i++) {
        Tot_num_Neighs[d*rows+i] = posn_to_neigh[i];
      }
      delete[] cell;
    } //End of atom1 loop
    ////----------------ALL Neighs have been found---////
    
    ////----------------Find Representation---------////
    //Create array to store representation of this batch
    //Assign Representation
    //atomic_Reprn = std::vector<double> (batch_size*reprn_length);
    if (atomic_Reprn.empty()) {
      atomic_Reprn = std::vector<double> (batch_size*reprn_length, 0);
    }
    //Clear and shrink any existing atomic_Reprn
    else {
      atomic_Reprn.clear();
      atomic_Reprn.shrink_to_fit();
      atomic_Reprn.resize(batch_size*reprn_length);
      std::fill(atomic_Reprn.begin(), atomic_Reprn.end(), 0);
    }
    
    ////----------------Find 2b Representation------////
    //2b representation is of type atomic, ie each atom has representation
    //In the end all atomic representation are added for 
    //atoms in the same crystal giving the crystal representation
    //ie batch representation
    //NOTE: This crystal representation can be incomplete as atoms 
    //from the same crystal can be split over different batches

    ////2b loop
    //Loop over central atoms in this batch
    for (int atom1=batch_start; atom1<batch_end; atom1++) {
      int d = atom1-batch_start; //d=0 is the first atom in this batch
      int Z1 = atoms_array_un(atom1,0);
      for (int i=0; i<nelements; i++)
         if (Z1 == elements[i])
           atomic_Reprn[d*reprn_length+1+i] += 1;
      
      //loop over interactions
      int n2b_interactions = num_of_interxns[0]; //total 2-body interactions
      int basis_start_posn = (1 + nelements);
      for (int interxn=0; interxn<n2b_interactions; interxn++){
        const std::vector<double> &knots = n2b_knots_array[interxn];
        const int num_knots = n2b_num_knots_array[interxn];
        /*if (!return_Neigh)
          py::print("basis_start_posn",basis_start_posn,"d =",d, d*reprn_length);*/
        const int num_neighs = Tot_num_Neighs[d*rows+interxn];

        //loop over all neighs of atom1 for interxn
        for (int atom2=0; atom2<num_neighs; atom2++){ 
          double r = Neighs[d*(rows*cols)+(interxn*cols)+atom2];
          double rsq = r*r;
          double rth = rsq*r;
        
          int knot_posn = num_knots-4;
          while (r<=knots[knot_posn])
            knot_posn--;

          int basis_posn = basis_start_posn+knot_posn;
          /*if (!return_Neigh)
            py::print(d*reprn_length+basis_posn-3);*/
          atomic_Reprn[d*reprn_length+basis_posn] += 
              //atomic_Reprn[d*reprn_length+knot_posn] +
              (constants_2b[interxn][knot_posn][0] +
              (r*constants_2b[interxn][knot_posn][1]) +
              (rsq*constants_2b[interxn][knot_posn][2]) +
              (rth*constants_2b[interxn][knot_posn][3]));

          atomic_Reprn[d*reprn_length+basis_posn-1] += 
              //atomic_Reprn[d*reprn_length+knot_posn-1] +
              (constants_2b[interxn][knot_posn-1][4] +
              (r*constants_2b[interxn][knot_posn-1][5]) +
              (rsq*constants_2b[interxn][knot_posn-1][6]) +
              (rth*constants_2b[interxn][knot_posn-1][7]));

          atomic_Reprn[d*reprn_length+basis_posn-2] += 
              //atomic_Reprn[d*reprn_length+knot_posn-2] +
              (constants_2b[interxn][knot_posn-2][8] +
              (r*constants_2b[interxn][knot_posn-2][9]) +
              (rsq*constants_2b[interxn][knot_posn-2][10]) +
              (rth*constants_2b[interxn][knot_posn-2][11]));

          atomic_Reprn[d*reprn_length+basis_posn-3] +=
              //atomic_Reprn[d*reprn_length+knot_posn-3] +
              (constants_2b[interxn][knot_posn-3][12] +
              (r*constants_2b[interxn][knot_posn-3][13]) +
              (rsq*constants_2b[interxn][knot_posn-3][14]) +
              (rth*constants_2b[interxn][knot_posn-3][15]));

        }// End of loop over neighs of atom1 for interxn
        basis_start_posn += (num_knots-4);
      }// End of interx loop
    } //End of atom1 loop

    //Add all atomic representation of atoms that part of the same crystal
    //total crystals in this batch
    int crystal_start = crystal_index_un(batch_start);
    ////batch_end-1 is the last atom of this batch
    int crystal_end = crystal_index_un(batch_end-1);
    //py::print("crystal_start =",crystal_start, "crystal_end =",crystal_end);

    int tot_crystals = crystal_end - crystal_start + 1; // +1 as indexing start from 0
    //py::print("tot_crystals =",tot_crystals, "batch_start =",batch_start,
    //        "batch_end =",batch_end);
    
    if (crystal_Reprn.empty()) {
      crystal_Reprn = std::vector<double> (tot_crystals*reprn_length);
    }
    //Clear and shrink any existing crystal_Reprn
    else {
      crystal_Reprn.clear();
      crystal_Reprn.shrink_to_fit();
      crystal_Reprn.resize(tot_crystals*reprn_length);
      std::fill(crystal_Reprn.begin(), crystal_Reprn.end(), 0);
    }

    //incomplete crystal representation from the previous batch
    //Add to the very first crystal
    if (!incomplete_crystal_Reprn.empty()){
      for (int i=0; i<reprn_length; i++)
        crystal_Reprn[i] = incomplete_crystal_Reprn[i];
    }
    
    for (int atom1=batch_start; atom1<batch_end; atom1++) {
      int d = atom1-batch_start; //d=0 is the first atom in this batch
      //index for crystal_Repren --> dC
      int dC = crystal_index_un(atom1) - crystal_start; //dC=0 is the first crystal
                                                        //in this batch
      for (int i=0; i<reprn_length; i++)
        crystal_Reprn[dC*reprn_length+i] += atomic_Reprn[d*reprn_length+i];
      //py::print("Num Nb=",crystal_Reprn[dC*reprn_length+1]);
    }
    //crystal_Reprn will have atmost 1 incomplete crystal representation
    //If it has incomplete crystal representation it will be the last crystal
    //ie crystal_Reprn[-1]

    //Determine if the last crystal representation is incomplete
    //Create incomplete_crystal_Reprn
    if (incomplete_crystal_Reprn.empty())
      incomplete_crystal_Reprn = std::vector<double> (1*reprn_length,0);
    else
      incomplete_crystal_Reprn.clear();
      incomplete_crystal_Reprn.shrink_to_fit();
      incomplete_crystal_Reprn.resize(1*reprn_length);
      std::fill(incomplete_crystal_Reprn.begin(), 
              incomplete_crystal_Reprn.end(), 0);

    ////batch_end-1 is the last atom of this batch
    int last_crystal_CI = crystal_index_un(batch_end-1);
    int start_posn = geom_posn_un(last_crystal_CI);
    int end_posn = geom_posn_un(last_crystal_CI+1);
    int num_atoms = end_posn-start_posn; //-->total num of atoms in the last
                                         //crystal of this batch

    int dC = crystal_index_un(batch_end-1) - crystal_start; 
    //dC=0 is the first crystal in this batch
    int num_atoms_from_CR = 0; //Num of atoms deduced from the last crystal
                               //representation
    //py::print("batch_number =",batch_numb);
    for (int i=0; i<nelements; i++) {
      num_atoms_from_CR += static_cast<int>(
                            std::ceil(
                                crystal_Reprn[(tot_crystals-1)*reprn_length+1+i]));
      //py::print("b=",crystal_Reprn[(tot_crystals-1)*reprn_length+i],
      //        "a=",crystal_Reprn[(tot_crystals-1)*reprn_length+2+i]);
      //py::print("CR =",num_atoms_from_CR);
    }
    //py::print("num_atoms =",num_atoms);
    if (num_atoms_from_CR!=num_atoms) //last crystal representation is incomplete
      for (int i=0; i<reprn_length; i++)
        incomplete_crystal_Reprn[i] = 
            crystal_Reprn[(tot_crystals-1)*reprn_length+i];
    

    //Write all complete crystal representations to hdf5 file
    if (tot_crystals==1) {//Just 1 crystal in this batch
      if (num_atoms_from_CR==num_atoms) //crystal representation complete
        //write hdf5
        write_hdf5(1, reprn_length, batch_numb,
                   feature_file, crystal_Reprn);
        /*write_hdf5(const hsize_t num_rows=1, const hsize_t num_cols=reprn_length,
                    const int batch_num=batch_numb, 
                    const H5::H5File &file_fp=feature_file,
                    const std::vector<double> &Data=crystal_Reprn);*/
    }
    else{
        //write hdf5 from complete crystal representation in this batch
        hsize_t h5_num_rows = tot_crystals;
        if (num_atoms_from_CR!=num_atoms)
          h5_num_rows = h5_num_rows-1;
        write_hdf5(h5_num_rows, reprn_length,
                   batch_numb, feature_file,
                   crystal_Reprn);
    }
  } //Loop over batch
  
  feature_file.close();

  if (return_Neigh){
    py::buffer_info Neigh_buff_info(
      Neighs.data(),      /* Pointer to buffer */
      sizeof(double),     /* Size of one scalar */
      py::format_descriptor<double>::format(),    /* Python struct-style format descriptor */
      3,                  /* Number of dimensions */
      { depth, rows, cols },  /* Buffer dimensions */
      { sizeof(double) * rows * cols,        /* Strides (in bytes) for each index */
      sizeof(double) * cols, 
      sizeof(double) }
    );
    return py::array(Neigh_buff_info);
  }
  
  else{
    py::buffer_info atomic_Reprn_buff_info(
      atomic_Reprn.data(),
      sizeof(double),
      py::format_descriptor<double>::format(),
      2,
      { batch_size, reprn_length},
      { sizeof(double)*reprn_length,
      sizeof(double) }
    );
    return py::array(atomic_Reprn_buff_info);
  }

}

py::array UltraFastFeaturize::get_elements()
{
  py::buffer_info elements_buff_info(
    elements.data(),
    sizeof(int),
    py::format_descriptor<int>::format(),
    1,
    {nelements},
    {sizeof(int)}
  );
  return py::array(elements_buff_info);
}

std::string UltraFastFeaturize::get_filename()
{
  return filename;
}

void UltraFastFeaturize::write_hdf5(const hsize_t num_rows, const hsize_t num_cols,
                               const int batch_num, const H5::H5File &file_fp,
                               const std::vector<double> &Data)
{
  // Create a unique dataset name for each batch
  std::string dataset_name = "feature_" + std::to_string(write_hdf5_counter);

  // Create the data space for the dataset
  hsize_t dims[2] = {num_rows, num_cols};

  H5::DataSpace data_space(2, dims);

  H5::DataSet data_set = file_fp.createDataSet(dataset_name,
                                               H5::PredType::NATIVE_DOUBLE,
                                               data_space);
  // Write data to the dataset from the vector
  data_set.write(Data.data(), H5::PredType::NATIVE_DOUBLE);

  // Close the dataset
  data_set.close();
  data_space.close();
  write_hdf5_counter++;
}

/*void UltraFastFeaturize::evaluate_2b(const double &r, const double &rsq,
                                const double &rth, const int &num_knots,
                                const std::vector<double> &knots)
{
}*/

/*
py::list convert_vector_of_vectors(const std::vector<std::vector<int>>& input) {
    py::list list;
    for (const auto& vec : input) {
        list.append(py::cast(vec)); // Convert each std::vector<int> to a NumPy array
    }
    return list;
}*/