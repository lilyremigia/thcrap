/**
  * Touhou Community Reliant Automatic Patcher
  * Main DLL
  *
  * ----
  *
  * Games search on disk
  */

#include <thcrap.h>
#include <filesystem>
#include <set>
#include <algorithm>
namespace fs = std::filesystem;

typedef struct alignas(16) {
	size_t size_min;
	size_t size_max;
	json_t *versions;
	std::vector<game_search_result> found;
	std::set<std::string> previously_known_games;
	CRITICAL_SECTION cs_result;
} search_state_t;

struct search_thread_param
{
	search_state_t *state;
	const wchar_t *dir;
};

static int SearchCheckExe(search_state_t& state, const fs::directory_entry &ent)
{
	int ret = 0;
	game_version *ver = identify_by_size((size_t)ent.file_size(), state.versions);
	if(ver) {
		std::string exe_fn = ent.path().generic_u8string();
		size_t file_size = (size_t)ent.file_size();
		identify_free(ver);
		ver = identify_by_hash(exe_fn.c_str(), &file_size, state.versions);
		if(!ver) {
			return ret;
		}

		// Check if user already selected a version of this game in a previous search
		if(state.previously_known_games.count(ver->id) > 0) {
			identify_free(ver);
			return ret;
		}

		// Alright, found a game!
		// Check if it has a vpatch
		auto vpatch_fn = ent.path().parent_path() / L"vpatch.exe";
		bool use_vpatch = false;
		if (strstr(ver->id, "_custom") == nullptr && fs::is_regular_file(vpatch_fn)) {
			use_vpatch = true;
		}

		EnterCriticalSection(&state.cs_result);
		{
			std::string description;
			if (ver->build || ver->variety) {
				if (ver->build)                 description += ver->build;
				if (ver->build && ver->variety) description += " ";
				if (ver->variety)               description += ver->variety;
			}

			game_search_result result = std::move(*ver);
			result.path = strdup(exe_fn.c_str());
			result.description = strdup(description.c_str());
			identify_free(ver);

			state.found.push_back(result);

			if (use_vpatch) {
				std::string vpatch_path = vpatch_fn.generic_u8string();
				if (std::none_of(state.found.begin(), state.found.end(), [vpatch_path](const game_search_result& it) {
					return vpatch_path == it.path;
					})) {
					game_search_result result_vpatch = result.copy();
					free(result_vpatch.path);
					free(result_vpatch.description);
					result_vpatch.path = strdup(vpatch_path.c_str());
					result_vpatch.description = strdup("using vpatch");
					state.found.push_back(result_vpatch);
				}
			}
		}
		LeaveCriticalSection(&state.cs_result);
		ret = 1;
	}
	return ret;
}


static DWORD WINAPI SearchThread(void *param_)
{
	search_thread_param *param = (search_thread_param*)param_;
	const wchar_t *dir = param->dir;
	search_state_t *state = param->state;
	delete param;
	try {
		for (auto &ent : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
			try {
				if (ent.is_regular_file()
					&& PathMatchSpecW(ent.path().c_str(), L"*.exe")
					&& ent.file_size() >= state->size_min
					&& ent.file_size() <= state->size_max)
				{
					SearchCheckExe(*state, ent);
				}
			}
			catch (fs::filesystem_error &) {}
		}
	} catch (fs::filesystem_error &) {}

	return 0;
}

static HANDLE LaunchSearchThread(search_state_t& state, const wchar_t *dir)
{
	search_thread_param *param = new search_thread_param{&state, dir};
	DWORD thread_id;
	return CreateThread(NULL, 0, SearchThread, param, 0, &thread_id);
}

// Used in std::sort. Return true if a < b.
bool compare_search_results(const game_search_result& a, const game_search_result& b)
{
	int cmp = strcmp(a.id, b.id);
	if (cmp < 0)
		return true;
	if (cmp > 0)
		return false;

	if (a.build && b.build) {
		// We want the higher version first
		cmp = strcmp(a.build, b.build);
		if (cmp > 0)
			return true;
		if (cmp < 0)
			return false;
	}

	if (a.description && b.description) {
		bool a_not_recommended = strstr(a.description, "(not recommended)");
		bool b_not_recommended = strstr(b.description, "(not recommended)");
		if (!a_not_recommended && b_not_recommended)
			return true;
		if (a_not_recommended && !b_not_recommended)
			return false;

		bool a_is_vpatch = strstr(a.description, "using vpatch");
		bool b_is_vpatch = strstr(b.description, "using vpatch");
		if (a_is_vpatch && !b_is_vpatch)
			return true;
		if (!a_is_vpatch && b_is_vpatch)
			return false;

		bool a_is_original = strstr(a.description, "original");
		bool b_is_original = strstr(b.description, "original");
		if (a_is_original && !b_is_original)
			return true;
		if (!a_is_original && b_is_original)
			return false;
	}

	// a == b
	return false;
}

game_search_result* SearchForGames(const wchar_t *dir, const games_js_entry *games_in)
{
	search_state_t state;
	const char *versions_js_fn = "versions.js";

	state.versions = stack_json_resolve(versions_js_fn, NULL);
	if(!state.versions) {
		log_printf(
			"ERROR: No version definition file (%s) found!\n"
			"Seems as if base_tsa didn't download correctly.\n"
			"Try deleting the thpatch directory and running this program again.\n", versions_js_fn
		);
		return NULL;
	}

	// Get file size limits
	json_t *sizes = json_object_get(state.versions, "sizes");
	// Error...
	state.size_min = -1;
	state.size_max = 0;
	const char *key;
	json_t *val;
	json_object_foreach(sizes, key, val) {
		size_t cur_size = atoi(key);

		if (cur_size < state.size_min)
			state.size_min = cur_size;
		if (cur_size > state.size_max)
			state.size_max = cur_size;
	}

	for (int i = 0; games_in && games_in[i].id; i++) {
		state.previously_known_games.insert(games_in[i].id);
	}

	InitializeCriticalSection(&state.cs_result);

	constexpr size_t max_threads = 32;
	HANDLE threads[max_threads];
	DWORD count = 0;

	if(dir && dir[0]) {
		if ((threads[count] = LaunchSearchThread(state, dir)) != NULL)
			count++;
	} else {
		wchar_t drive_strings[512];
		wchar_t *p = drive_strings;

		GetLogicalDriveStringsW(512, drive_strings);
		while(p && p[0] && count < max_threads) {
			UINT drive_type = GetDriveTypeW(p);
			if(
				(drive_type != DRIVE_CDROM) &&
				(p[0] != L'A') &&
				(p[0] != L'a')
			) {
				if ((threads[count] = LaunchSearchThread(state, p)) != NULL)
					count++;
			}
			p += wcslen(p) + 1;
		}
	}
	WaitForMultipleObjects(count, threads, TRUE, INFINITE);
	while (count)
		CloseHandle(threads[--count]);

	DeleteCriticalSection(&state.cs_result);
	json_decref(state.versions);

	std::sort(state.found.begin(), state.found.end(), compare_search_results);
	game_search_result *ret = (game_search_result*)malloc((state.found.size() + 1) * sizeof(game_search_result));
	size_t i;
	for (i = 0; i < state.found.size(); i++) {
		ret[i] = state.found[i];
	}
	memset(&ret[i], 0, sizeof(game_search_result));
	return ret;
}

void SearchForGames_free(game_search_result *games)
{
	if (!games)
		return;
	for (size_t i = 0; games[i].id; i++) {
		free(games[i].path);
		free(games[i].id);
		free(games[i].build);
		free(games[i].description);
	}
	free(games);
}
