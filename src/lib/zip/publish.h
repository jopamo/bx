#ifndef ZU_PUBLISH_H
#define ZU_PUBLISH_H

char* zu_publish_make_temp_path(const char* temp_dir, const char* target_path);
int zu_publish_replace(const char* source_path, const char* target_path);

#endif /* ZU_PUBLISH_H */
