# Contains helpful function for Ultra Fast Featurization
# More documentation will be added

import numpy as np

from uf3.data.geometry import get_supercell_factors
import ase
from ase import symbols as ase_symbols

def get_interactions_map_ff(interactions_map):
    interactions_map_ff = []

    for n1body in interactions_map[1]:
        interactions_map_ff.append(ase_symbols.atomic_numbers[n1body])

    for n2body in interactions_map[2]:
        interactions_map_ff.append((ase_symbols.atomic_numbers[n2body[0]],
                                    ase_symbols.atomic_numbers[n2body[1]]))

    if 3 in interactions_map.keys():
        for n3body in interactions_map[3]:
            interactions_map_ff.append((ase_symbols.atomic_numbers[n3body[0]],
                                        ase_symbols.atomic_numbers[n3body[1]],
                                        ase_symbols.atomic_numbers[n3body[2]]))

    return tuple(interactions_map_ff)

def get_2b_knots_map_ff(bspline_config):
    max_num_knots = 0
    for i in bspline_config.interactions_map[2]:
        max_num_knots = max(len(bspline_config.knots_map[i]),
                            max_num_knots)

    knots_map_ff = np.zeros((len(bspline_config.interactions_map[2]),max_num_knots))


    for i, pair in enumerate(bspline_config.interactions_map[2]):
        shape0 = bspline_config.knots_map[pair].shape[0]
        knots_map_ff[i][0:shape0] = bspline_config.knots_map[pair]

    return knots_map_ff

def get_2b_num_knots_ff(bspline_config):
    knots_size_2b_ff = np.zeros(len(bspline_config.interactions_map[2]),dtype=np.int32)

    for i, pair in enumerate(bspline_config.interactions_map[2]):
        knots_size_2b_ff[i] = bspline_config.knots_map[pair].shape[0]

    return knots_size_2b_ff

def convert_ase_atom_to_array(ase_atom):
    #cell = np.concatenate([[[0],[0],[0]], ase_atom.cell.array],axis=1)
    shape_0 = ase_atom.get_atomic_numbers().shape[0]
    species_pos = np.concatenate([ase_atom.get_atomic_numbers().reshape((shape_0,1)),
                                  ase_atom.positions], axis=1)
    return species_pos

def get_atoms_array(df):
    ase_atom = df['geometry']
    return convert_ase_atom_to_array(ase_atom)

def get_crystal_index(df):
    return np.full(df['geometry'].get_global_number_of_atoms(), fill_value=df['crystal_index'])

def get_cells(df):
    return df['geometry'].cell.array

def get_geom_array_posn(df):
    return df['geometry_array'].shape(0)

def get_scell_factors(df, bspline_config):
    cell = df['geometry'].cell
    r_cut = bspline_config.r_cut
    return get_supercell_factors(cell, r_cut).astype(np.int32)


def get_supercell_array(df, bspline_config):
    ase_atom = df['geometry']
    supercell = get_supercell(ase_atom, bspline_config.r_cut)
    return convert_ase_atom_to_array(supercell)

def get_data_for_UltraFastFeaturization(bspline_config, df):
    chemical_system = bspline_config.chemical_system
    interactions_map_ff = get_interactions_map_ff(chemical_system.interactions_map)
    n2b_knots_map_ff = get_2b_knots_map_ff(bspline_config)
    n2b_num_knots_ff = get_2b_num_knots_ff(bspline_config)

    df['atoms_array'] = df.apply(get_atoms_array,axis=1)
    atoms_array = np.concatenate(df['atoms_array'])

    df['crystal_index'] = range(0,len(df))
    df['crystal_index'] = df.apply(get_crystal_index,axis=1)
    crystal_index = np.concatenate(df['crystal_index'],axis=0).astype(np.int32)


    df['cell_array'] = df.apply(get_cells,axis=1)
    cell_array = np.stack(df['cell_array'])


    df['supercell_factors'] = df.apply(get_scell_factors,
                                                         axis=1,
                                                         args=[bspline_config])
    supercell_factors = np.stack(df['supercell_factors'])

    geom_array_posn = np.concatenate(df.apply(lambda x: [x['atoms_array'].shape[0]],
                                                     axis=1))
    geom_array_posn = np.cumsum(geom_array_posn)
    geom_array_posn = np.concatenate([[0], geom_array_posn]).astype(np.int32)

    return [interactions_map_ff, n2b_knots_map_ff, n2b_num_knots_ff,
            atoms_array, cell_array, crystal_index, supercell_factors, 
            geom_array_posn]