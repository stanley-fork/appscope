package rules

import (
	"errors"
	"io"
	"os"
	"path/filepath"

	"github.com/criblio/scope/libscope"
	"github.com/criblio/scope/loader"
	"github.com/criblio/scope/run"
	"github.com/criblio/scope/util"
	"github.com/rs/zerolog/log"
	"gopkg.in/yaml.v2"
)

var (
	errNoScopedProcs     = errors.New("no scoped processes found")
	errDetachingMultiple = errors.New("at least one error found when detaching from more than 1 process. See logs")
	errAttachingMultiple = errors.New("at least one error found when attaching to more than 1 process. See logs")
)

// Retrieve the rules file from the global location
func Retrieve(rootdir string) ([]byte, libscope.Rules, error) {
	rulesFile := make(libscope.Rules, 0)

	// TODO support for $CRIBL_HOME
	filePath := "/usr/lib/appscope/scope_rules"
	if rootdir != "" {
		filePath = rootdir + filePath
	}

	file, err := os.Open(filePath)
	if err != nil {
		if os.IsNotExist(err) {
			// Return an empty rules file if it does not exist
			return []byte{}, rulesFile, nil
		}
		return nil, nil, err
	}
	defer file.Close()

	data, err := io.ReadAll(file)
	if err != nil {
		return nil, nil, err
	}

	// Read yaml in
	err = yaml.Unmarshal(data, &rulesFile)
	if err != nil {
		util.Warn("Error unmarshaling YAML:%v", err)
		return nil, nil, err
	}

	return data, rulesFile, nil
}

// Add a process to the scope rules
func Add(rulesFile libscope.Rules, addProc, procArg, sourceid, rootdir string, rc *run.Config) error {

	// Create a history directory for logs
	rc.CreateWorkDirBasic("rules")

	// Instantiate the loader
	ld := loader.New()

	////////////////////////////////////////////
	// Install libscope if not already installed
	////////////////////////////////////////////

	stdoutStderr, err := ld.Install(rootdir)
	if err != nil {
		log.Warn().
			Err(err).
			Str("loaderDetails", stdoutStderr).
			Msg("Install library failed.")
		return err
	} else {
		log.Info().
			Msg("Install library success.")
	}

	////////////////////////////////////////////
	// Update the global rules file
	////////////////////////////////////////////

	// Build the config
	rc.LoadConfig(true)

	// Add the entry to the rules
	newEntry := libscope.Entry{
		ProcName:      addProc,
		ProcArg:       procArg,
		SourceId:      sourceid,
		Configuration: rc.GetScopeConfig(),
	}
	rulesFile["allow"] = append(rulesFile["allow"], newEntry)

	// Write the rules contents to a temporary path
	rulesFilePath := "/tmp/scope_rules"
	file, err := os.OpenFile(rulesFilePath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		log.Warn().Err(err).Msgf("Error creating/opening %s", rulesFilePath)
		return err
	}
	defer file.Close()

	data, err := yaml.Marshal(&rulesFile)
	if err != nil {
		util.Warn("Error marshaling YAML:%v", err)
		return err
	}

	_, err = file.Write(data)
	if err != nil {
		util.Warn("Error writing to file:%v", err)
		return err
	}

	// Ask the loader to update the rules file
	if stdoutStderr, err := ld.Rules(rulesFilePath, rc.Rootdir); err != nil {
		log.Warn().
			Err(err).
			Str("loaderDetails", stdoutStderr).
			Msgf("Install library in %s namespace failed.", rootdir)
		return err
	}

	////////////////////////////////////////////
	// Set ld.so.preload if it is not already set.
	////////////////////////////////////////////

	if stdoutStderr, err := ld.Preload("auto", rc.Rootdir); err != nil {
		log.Warn().
			Err(err).
			Str("loaderDetails", stdoutStderr).
			Msgf("Install library in %s namespace failed.", rootdir)
		return err
	}

	////////////////////////////////////////////
	// Attach to all matching processes that are not already scoped
	// (will re-attach to update existing procs)
	////////////////////////////////////////////

	// Create config file in the rules workdir for all processes to be attached
	scYamlPath := filepath.Join(rc.WorkDir, "scope.yml")
	if err := rc.WriteScopeConfig(scYamlPath, 0644); err != nil {
		return err
	}

	if rootdir != "" {
		util.Warn("It can take up to 1 minute to attach to processes in a parent namespace")
	}

	procs, err := util.HandleInputArg(addProc, procArg, rc.Rootdir, false, false, true, true)
	if err != nil {
		return err
	}
	// Note: if len(procs) == 0 do nothing
	if len(procs) == 1 {
		if _, err = rc.Attach(procs[0].Pid, false); err != nil {
			return err
		}
	} else if len(procs) > 1 {
		errors := false
		for _, proc := range procs {
			if _, err = rc.Attach(proc.Pid, false); err != nil {
				log.Error().Err(err)
				errors = true
			}
		}
		if errors {
			return errAttachingMultiple
		}
	}

	// TBC ? perform a scope detach to all processes that do not match anything in the rules file

	////////////////////////////////////////////
	// Apply rules to existing containers
	////////////////////////////////////////////

	/* TODO

	find all existing containers

	for each container:

		// Mount `scope_rules` into the container
		// TODO support CRIBL_HOME
		ld.Mount()
			scope --mount (/usr/lib/appscope/, <container root>/usr/lib/appscope) --rootdir /hostfs

		// Mount unix socket into the container
		determine <unix_socket_dir> by looking in the filter for the unix socket path
		ld.Mount()
			scope --mount (<unix_socket_dir>, <container root>/<unix_socket_dir>) --rootdir /hostfs

		// Extract the library to /opt/appscope/libscope.so
		scope extract /opt/appscope --rootdir <container root>

		// Set ld.preload.so to point to /opt/appscope/libscope.so
		ld.Preload()
			scope --preload /opt/appscope --rootdir <container root>                        #

		// Attach is already taken care of, we attached to everything in rootdir and children
	*/

	return nil
}

// Remove a process from the scope rules
// Note: No matching of the 'arg' field intended for removal.
func Remove(rulesFile libscope.Rules, remProc, sourceid, rootdir string, rc *run.Config) error {

	// Create a history directory for logs
	rc.CreateWorkDirBasic("rules")

	// Instantiate the loader
	ld := loader.New()

	////////////////////////////////////////////
	// Update the global rules file
	////////////////////////////////////////////

	// Remove the entry from the rules
	// (only the last instance matching the name)
	// (only if there are changes)
	if allowList, ok := rulesFile["allow"]; ok {
		remove := -1
		for i, entry := range allowList {
			if entry.ProcName == remProc && entry.SourceId == sourceid {
				remove = i
			}
		}
		if remove > -1 {
			allowList = append(allowList[:remove], allowList[remove+1:]...)
			rulesFile["allow"] = allowList // copy back the allowList

			// Write the rules contents to a temporary path
			rulesFilePath := "/tmp/scope_rules"
			file, err := os.OpenFile(rulesFilePath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0644)
			if err != nil {
				log.Warn().Err(err).Msgf("Error creating/opening %s", rulesFilePath)
				return err
			}
			defer file.Close()

			data, err := yaml.Marshal(&rulesFile)
			if err != nil {
				util.Warn("Error marshaling YAML:%v", err)
				return err
			}

			_, err = file.Write(data)
			if err != nil {
				util.Warn("Error writing to file:%v", err)
				return err
			}

			// Ask the loader to update the rules file
			if stdoutStderr, err := ld.Rules(rulesFilePath, rc.Rootdir); err != nil {
				log.Warn().
					Err(err).
					Str("loaderDetails", stdoutStderr).
					Msgf("Install library in %s namespace failed.", rootdir)
				return err
			}
		}
	}

	////////////////////////////////////////////
	// Unset ld.so.preload if it is already set.
	// Only if this is the last entry in the file
	////////////////////////////////////////////

	if len(rulesFile["allow"]) == 0 {
		if stdoutStderr, err := ld.Preload("off", rc.Rootdir); err != nil {
			log.Warn().
				Err(err).
				Str("loaderDetails", stdoutStderr).
				Msgf("Install library in %s namespace failed.", rootdir)
			return err
		}
	}

	////////////////////////////////////////////
	// Detach from all matching, scoped processes
	////////////////////////////////////////////

	procs, err := util.HandleInputArg(remProc, "", rc.Rootdir, false, false, false, true)
	if err != nil {
		return err
	}
	// Note: if len(procs) == 0 do nothing
	if len(procs) == 1 {
		if err = rc.Detach(procs[0].Pid); err != nil {
			return err
		}
	} else if len(procs) > 1 {
		errors := false
		for _, proc := range procs {
			if err = rc.Detach(proc.Pid); err != nil {
				log.Error().Err(err)
				errors = true
			}
		}
		if errors {
			return errDetachingMultiple
		}
	}

	return nil
}
