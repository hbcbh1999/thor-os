//=======================================================================
// Copyright Baptiste Wicht 2013.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

#include "fat32.hpp"
#include "console.hpp"

#include "stl/types.hpp"
#include "stl/unique_ptr.hpp"
#include "stl/algorithms.hpp"
#include "stl/pair.hpp"

namespace {

//FAT 32 Boot Sector
struct fat_bs_t {
    uint8_t jump[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t number_of_fat;
    uint16_t root_directories_entries;
    uint16_t total_sectors;
    uint8_t media_descriptor;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_long;
    uint32_t sectors_per_fat_long;
    uint16_t drive_description;
    uint16_t version;
    uint32_t root_directory_cluster_start;
    uint16_t fs_information_sector;
    uint16_t boot_sectors_copy_sector;
    uint8_t filler[12];
    uint8_t physical_drive_number;
    uint8_t reserved;
    uint8_t extended_boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char file_system_type[8];
    uint8_t boot_code[420];
    uint16_t signature;
}__attribute__ ((packed));

struct fat_is_t {
    uint32_t signature_start;
    uint8_t reserved[480];
    uint32_t signature_middle;
    uint32_t free_clusters;
    uint32_t allocated_clusters;
    uint8_t reserved_2[12];
    uint32_t signature_end;
}__attribute__ ((packed));

static_assert(sizeof(fat_bs_t) == 512, "FAT Boot Sector is exactly one disk sector");

struct cluster_entry {
    char name[11];
    uint8_t attrib;
    uint8_t reserved;
    uint8_t creation_time_seconds;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t accessed_date;
    uint16_t cluster_high;
    uint16_t modification_time;
    uint16_t modification_date;
    uint16_t cluster_low;
    uint32_t file_size;
} __attribute__ ((packed));

static_assert(sizeof(cluster_entry) == 32, "A cluster entry is 32 bytes");

uint64_t cached_disk = -1;
uint64_t cached_partition = -1;
uint64_t partition_start;

fat_bs_t* fat_bs = nullptr;
fat_is_t* fat_is = nullptr;

void cache_bs(fat32::dd disk, const disks::partition_descriptor& partition){
    std::unique_ptr<fat_bs_t> fat_bs_tmp(new fat_bs_t());

    if(read_sectors(disk, partition.start, 1, fat_bs_tmp.get())){
        fat_bs = fat_bs_tmp.release();

        //TODO fat_bs->signature should be 0xAA55
        //TODO fat_bs->file_system_type should be FAT32
    } else {
        fat_bs = nullptr;
    }
}

void cache_is(fat32::dd disk, const disks::partition_descriptor& partition){
    auto fs_information_sector = partition.start + static_cast<uint64_t>(fat_bs->fs_information_sector);

    std::unique_ptr<fat_is_t> fat_is_tmp(new fat_is_t());

    if(read_sectors(disk, fs_information_sector, 1, fat_is_tmp.get())){
        fat_is = fat_is_tmp.release();

        //TODO fat_is->signature_start should be 0x52 0x52 0x61 0x41
        //TODO fat_is->signature_middle should be 0x72 0x72 0x41 0x61
        //TODO fat_is->signature_end should be 0x00 0x00 0x55 0xAA
    } else {
        fat_is = nullptr;
    }
}

uint64_t cluster_lba(uint64_t cluster){
    uint64_t fat_begin = partition_start + fat_bs->reserved_sectors;
    uint64_t cluster_begin = fat_begin + (fat_bs->number_of_fat * fat_bs->sectors_per_fat_long);

    return cluster_begin + (cluster - 2 ) * fat_bs->sectors_per_cluster;
}

uint32_t read_fat_value(fat32::dd disk, uint32_t cluster){
    uint64_t fat_begin = partition_start + fat_bs->reserved_sectors;
    uint32_t cluster_size = 512 * fat_bs->sectors_per_cluster;

    uint64_t fat_sector = fat_begin + (cluster * 4) / cluster_size;

    std::unique_heap_array<uint32_t> fat_table(cluster_size / sizeof(uint32_t));
    if(read_sectors(disk, fat_sector, fat_bs->sectors_per_cluster, fat_table.get())){
        uint64_t entry_offset = ((cluster * 4) % cluster_size) / 4;
        return fat_table[entry_offset] & 0x0FFFFFFF;
    } else {
        return 0;
    }
}

uint32_t next_cluster(fat32::dd disk, uint32_t cluster){
    auto fat_value = read_fat_value(disk, cluster);
    if(fat_value >= 0x0FFFFFF8){
        return 0;
    }

    return fat_value;
}

inline bool entry_used(const cluster_entry& entry){
    return static_cast<unsigned char>(entry.name[0]) != 0xE5;
}

inline bool end_of_directory(const cluster_entry& entry){
    return entry.name[0] == 0x0;
}

inline bool is_long_name(const cluster_entry& entry){
    return entry.attrib == 0x0F;
}

size_t filename_length(char* filename){
    for(size_t s = 0; s < 11; ++s){
        if(filename[s] == ' '){
            return s;
        }
    }

    return 11;
}

bool filename_equals(char* name, const std::string& path){
    auto length = filename_length(name);

    if(path.size() != length){
        return false;
    }

    for(size_t i = 0; i < length; ++i){
        if(path[i] != name[i]){
            return false;
        }
    }

    return true;
}

bool cache_disk_partition(fat32::dd disk, const disks::partition_descriptor& partition){
    if(cached_disk != disk.uuid || cached_partition != partition.uuid){
        partition_start = partition.start;

        cache_bs(disk, partition);
        cache_is(disk, partition);

        cached_disk = disk.uuid;
        cached_partition = partition.uuid;
    }

    //Something may go wrong when reading the two base vectors
    return fat_bs && fat_is;
}

std::pair<bool, uint32_t> find_cluster_number(fat32::dd disk, const std::vector<std::string>& path){
    if(path.empty()){
        return std::make_pair(true, static_cast<uint32_t>(fat_bs->root_directory_cluster_start));
    }

    auto cluster_number = fat_bs->root_directory_cluster_start;
    auto cluster_addr = cluster_lba(cluster_number);

    std::unique_heap_array<cluster_entry> current_cluster(16 * fat_bs->sectors_per_cluster);

    if(read_sectors(disk, cluster_addr, fat_bs->sectors_per_cluster, current_cluster.get())){
        for(size_t i = 0; i < path.size(); ++i){
            auto& p = path[i];

            bool found = false;
            bool end_reached = false;

            while(!found){
                for(auto& entry : current_cluster){
                    if(end_of_directory(entry)){
                        end_reached = true;
                        break;
                    }

                    if(entry_used(entry) && !is_long_name(entry) && entry.attrib & 0x10){
                        //entry.name is not a real c_string, cannot be compared
                        //directly
                        if(filename_equals(entry.name, p)){
                            cluster_number = entry.cluster_low + (entry.cluster_high << 16);

                            //If it is the last part of the path, just return the
                            //number
                            if(i == path.size() - 1){
                                return std::make_pair(true, cluster_number);
                            }

                            //Otherwise, continue deeper in the search

                            if(read_sectors(disk, cluster_lba(cluster_number), fat_bs->sectors_per_cluster, current_cluster.get())){
                                found = true;

                                break;
                            } else {
                                return std::make_pair(false, 0);
                            }
                        }
                    }
                }

                //If not found, try the next cluster, if any
                if(!end_reached && !found){
                    auto next = next_cluster(disk, cluster_number);

                    //If there are no more cluster, return false
                    if(!next){
                        return std::make_pair(false, 0);
                    }

                    //The block is corrupted
                    if(next == 0x0FFFFFF7){
                        return std::make_pair(false, 0);
                    }


                    //Read the next cluster in the chain
                    cluster_number = next;
                    if(!read_sectors(disk, cluster_lba(cluster_number), fat_bs->sectors_per_cluster, current_cluster.get())){
                        return std::make_pair(false, 0);
                    }
                }
            }

            //If still not found at this point, return false
            if(!found){
                return std::make_pair(false, 0);
            }
        }
    }

    return std::make_pair(false, 0);
}

std::pair<bool, std::unique_heap_array<cluster_entry>> find_directory_cluster(fat32::dd disk, const std::vector<std::string>& path){
    auto cluster_number = find_cluster_number(disk, path);

    if(cluster_number.first){
        std::unique_heap_array<cluster_entry> cluster(16 * fat_bs->sectors_per_cluster);

        if(read_sectors(disk, cluster_lba(cluster_number.second), fat_bs->sectors_per_cluster, cluster.get())){
            return std::make_pair(true, std::move(cluster));
        } else {
            return std::make_pair(false, std::unique_heap_array<cluster_entry>());
        }
    }

    return std::make_pair(false, std::unique_heap_array<cluster_entry>());
}

std::vector<disks::file> files(fat32::dd disk, const std::vector<std::string>& path){
    auto cluster_number_search = find_cluster_number(disk, path);
    if(!cluster_number_search.first){
        return {};
    }

    std::vector<disks::file> files;

    bool end_reached = false;

    while(!end_reached){
        auto cluster_number = cluster_number_search.second;

        std::unique_heap_array<cluster_entry> cluster(16 * fat_bs->sectors_per_cluster);

        if(!read_sectors(disk, cluster_lba(cluster_number), fat_bs->sectors_per_cluster, cluster.get())){
            return std::move(files);
        }

        for(auto& entry : cluster){
            if(end_of_directory(entry)){
                end_reached = true;
                break;
            }

            if(entry_used(entry)){
                disks::file file;

                if(is_long_name(entry)){
                    //It is a long file name
                    //TODO Add suppport for long file name
                    file.file_name = "LONG";
                } else {
                    //It is a normal file name
                    //Copy the name until the first space

                    for(size_t s = 0; s < 11; ++s){
                        if(entry.name[s] == ' '){
                            break;
                        }

                        file.file_name += entry.name[s];
                    }
                }

                file.hidden = entry.attrib & 0x1;
                file.system = entry.attrib & 0x2;
                file.directory = entry.attrib & 0x10;

                if(file.directory){
                    file.size = fat_bs->sectors_per_cluster * 512;
                } else {
                    file.size = entry.file_size;
                }

                files.push_back(file);
            }
        }

        if(!end_reached){
            auto next = next_cluster(disk, cluster_number);

            //If there are no more cluster, return false
            if(!next){
                return std::move(files);
            }

            //The block is corrupted
            if(next == 0x0FFFFFF7){
                return std::move(files);
            }

            //Read the next cluster in the chain
            cluster_number = next;
        }
    }

    return std::move(files);
}

} //end of anonymous namespace

uint64_t fat32::free_size(dd disk, const disks::partition_descriptor& partition){
    if(!cache_disk_partition(disk, partition)){
        return 0;
    }

    return fat_is->free_clusters * fat_bs->sectors_per_cluster * 512;
}

std::vector<disks::file> fat32::ls(dd disk, const disks::partition_descriptor& partition, const std::vector<std::string>& path){
    if(!cache_disk_partition(disk, partition)){
        return {};
    }

    return files(disk, path);
}

std::string fat32::read_file(dd disk, const disks::partition_descriptor& partition, const std::vector<std::string>& path, const std::string& file){
    if(!cache_disk_partition(disk, partition)){
        return {};
    }

    auto result = find_directory_cluster(disk, path);

    if(result.first){
        auto& directory_cluster = result.second;

        bool found = false;
        bool end_reached = false;

        for(auto& entry : directory_cluster){
            if(end_of_directory(entry)){
                end_reached = true;
                break;
            }

            if(entry_used(entry) && !is_long_name(entry) && !(entry.attrib & 0x10)){
                if(filename_equals(entry.name, file)){
                    std::string content(entry.file_size + 1);

                    auto cluster = entry.cluster_low + (entry.cluster_high << 16);

                    size_t read = 0;

                    while(read < entry.file_size){
                        size_t cluster_size = 512 * fat_bs->sectors_per_cluster;
                        std::unique_heap_array<char> sector(cluster_size);

                        if(read_sectors(disk, cluster_lba(cluster), fat_bs->sectors_per_cluster, sector.get())){
                            for(size_t i = 0; i < cluster_size && read < entry.file_size; ++i,++read){
                                content += sector[i];
                            }
                        } else {
                            break;
                        }

                        //If the file is not read completely, get the next
                        //cluster
                        if(read < entry.file_size){
                            auto next = next_cluster(disk, cluster);

                            //It may be possible that either the file size or
                            //the FAT entry is wrong
                            if(!next){
                                break;
                            }

                            //The block is corrupted
                            if(next == 0x0FFFFFF7){
                                break;
                            }

                            cluster = next;
                        }
                    }

                    return std::move(content);
                }
            }
        }

        if(!found && end_reached){
            //TODO Read the next cluster to find the file
        }
    }

    return {};
}
