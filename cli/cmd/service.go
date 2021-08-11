package cmd

import (
	"bufio"
	"path/filepath"
	"fmt"
	"io/ioutil"
	"os"
	"strings"
	"syscall"
	"github.com/criblio/scope/util"
	"github.com/criblio/scope/run"
	"github.com/spf13/cobra"
)

var forceFlag bool
var serviceUser string

// servichCmd represents the service command
var serviceCmd = &cobra.Command{
	Use:   "service SERVICE [flags]",
	Short: "Configure a systemd service to be scoped",
	Long: `the "scope service" command adjusts the configuration for the named systemd service so it is scoped when it starts.`,
Example: `scope service  cribl -c tls://in.my-instance.cribl.cloud:10090`,
	Args: cobra.MinimumNArgs(1),
	Run: func(cmd *cobra.Command, args []string) {
		// must be root
		if 0 != os.Getuid() {
			util.ErrAndExit("error: must be run as root")
		}

		// service name is the first and only argument
		if len(args) < 1 {
			util.ErrAndExit("error: missing required SERVICE argument")
		}
		if len(args) > 1 {
			os.Stderr.WriteString("warn: ignoring extra arguments\n")
		}
		serviceName := args[0]

		// check for Systemd
		if _, err := os.Stat("/etc/systemd"); err != nil {
			util.ErrAndExit("error: Systemd required; missing /etc/systemd")
		}

		// the service name must exist
		serviceFile := ""
		filepath.Walk("/etc/systemd", func(path string, info os.FileInfo, err error) error {
			if err == nil && info.Name() == serviceName + ".service" {
				serviceFile = path + "/" + info.Name()
				return nil
			}
			return nil
		})
		if serviceFile == "" {
			util.ErrAndExit("error: didn't find service file; " + serviceName + ".service")
		} else {
			//os.Stdout.WriteString("info: found service file; " + serviceFile + "\n")
		}

		// get uname pieces
		utsname := syscall.Utsname{}
		err := syscall.Uname(&utsname)
		util.CheckErrSprintf(err, "error: syscall.Uname failed; %v", err)
		buf := make ([]byte,0,64)
		for _, v := range utsname.Machine[:] {
			if v == 0 { break }
			buf = append(buf, uint8(v))
		}
		unameMachine := string(buf)
		buf = make ([]byte,0,64)
		for _, v := range utsname.Sysname[:] {
			if v == 0 { break }
			buf = append(buf, uint8(v))
		}
		unameSysname := strings.ToLower(string(buf))

		// TODO get libc name
		libcName := "gnu"

		// get confirmation
		if !forceFlag {
			fmt.Printf("\nThis command will make the following changes if not found already:\n")
			fmt.Printf("  - install libscope.so into /usr/lib/%s-%s-%s/cribl/\n", unameMachine, unameSysname, libcName)
			fmt.Printf("  - create %s.d/env.conf override\n", serviceFile)
			fmt.Printf("  - create /etc/scope/%s/scope.yml\n", serviceName)
			fmt.Printf("  - create /var/log/scope/\n")
			fmt.Printf("  - create /var/run/scope/\n")
			if !confirm("Ready to proceed?") {
				util.ErrAndExit("info: canceled")
			}
		}

		// extract the library
		libraryDir := fmt.Sprintf("/usr/lib/%s-%s-%s/cribl", unameMachine, unameSysname, libcName)
		if _, err := os.Stat(libraryDir); err != nil {
			err := os.Mkdir(libraryDir, 0755)
			util.CheckErrSprintf(err, "error: failed to create library directory; %v", err)
			//os.Stdout.WriteString("info: created library directory; " + libraryDir + "\n")
		} else {
			//os.Stdout.WriteString("info: library directory exists; " + libraryDir + "\n")
		}
		libraryPath := libraryDir + "/libscope.so"
		if _, err := os.Stat(libraryPath); err != nil {
			asset, err := run.Asset("build/libscope.so")
			util.CheckErrSprintf(err, "error: failed to find libscope.so asset; %v", err)
			err = ioutil.WriteFile(libraryPath, asset, 0755)
			util.CheckErrSprintf(err, "error: failed to extract library; %v", err)
			//os.Stdout.WriteString("info: extracted library; " + libraryPath + "\n")
		} else {
			//os.Stdout.WriteString("info: library exists; " + libraryPath + "\n")
		}

		// create the config directory
		configBaseDir := fmt.Sprintf("/etc/scope")
		if _, err := os.Stat(configBaseDir); err != nil {
            err := os.Mkdir(configBaseDir, 0755)
            util.CheckErrSprintf(err, "error: failed to create config base directory; %v", err)
            //os.Stdout.WriteString("info: created config base directory; " + configBaseDir + "\n")
        } else {
            //os.Stdout.WriteString("info: config base directory exists; " + configBaseDir + "\n")
        }
		configDir := fmt.Sprintf("/etc/scope/%s", serviceName)
		if _, err := os.Stat(configDir); err != nil {
            err := os.Mkdir(configDir, 0755)
            util.CheckErrSprintf(err, "error: failed to create config directory; %v", err)
            //os.Stdout.WriteString("info: created config directory; " + configDir + "\n")
        } else {
            //os.Stdout.WriteString("info: config directory exists; " + configDir + "\n")
        }

		// create the log directory
		logDir := fmt.Sprintf("/var/log/scope")
		if _, err := os.Stat(logDir); err != nil {
            err := os.Mkdir(logDir, 0755) // TODO chown to who?
            util.CheckErrSprintf(err, "error: failed to create log directory; %v", err)
            //os.Stdout.WriteString("info: created log directory; " + logDir + "\n")
        } else {
            //os.Stdout.WriteString("info: log directory exists; " + logDir + "\n")
        }

		// create the run directory
		runDir := fmt.Sprintf("/var/run/scope")
		if _, err := os.Stat(logDir); err != nil {
            err := os.Mkdir(runDir, 0755) // TODO chown to who?
            util.CheckErrSprintf(err, "error: failed to create run directory; %v", err)
            //os.Stdout.WriteString("info: created run directory; " + runDir + "\n")
        } else {
            //os.Stdout.WriteString("info: run directory exists; " + runDir + "\n")
        }

		// extract scope.yml
		configPath := fmt.Sprintf("/etc/scope/%s/scope.yml", serviceName)
		if _, err := os.Stat(configPath); err == nil {
			util.ErrAndExit("error: scope.yml already exists; " + configPath)
		}
		asset, err := run.Asset("build/scope.yml")
		util.CheckErrSprintf(err, "error: failed to find scope.yml asset; %v", err)
		err = ioutil.WriteFile(configPath, asset, 0644)
		util.CheckErrSprintf(err, "error: failed to extract scope.yml; %v", err)
		if rc.MetricsDest != "" || rc.EventsDest != "" || rc.CriblDest != "" {
			examplePath := fmt.Sprintf("/etc/scope/%s/scope_example.yml", serviceName)
			err = os.Rename(configPath, examplePath)
			util.CheckErrSprintf(err, "error: failed to move scope.yml to soppe_example.yml; %v", err)
			rc.WorkDir = configDir
			rc.GetScopeConfig().Libscope.Log.Transport.Path = logDir + "/cribl.log"
			rc.GetScopeConfig().Libscope.CommandDir = runDir
			err = rc.WriteScopeConfig(configPath, 0644)
			util.CheckErrSprintf(err, "error: failed to create scope.yml: %v", err)
		}

		// extract scope_protocol.yml
		protocolPath := fmt.Sprintf("/etc/scope/%s/scope_protocol.yml", serviceName)
		if _, err := os.Stat(protocolPath); err == nil {
			util.ErrAndExit("error: scope_protocol.yml already exists; " + protocolPath)
		}
		asset, err = run.Asset("build/scope_protocol.yml")
		util.CheckErrSprintf(err, "error: failed to find scope_protocol.yml asset; %v", err)
		err = ioutil.WriteFile(protocolPath, asset, 0644)
		util.CheckErrSprintf(err, "error: failed to extract scope_protocol.yml; %v", err)

		// create service override
		overrideDir := fmt.Sprintf("%s.d", serviceFile)
		if _, err := os.Stat(overrideDir); err != nil {
            err := os.Mkdir(overrideDir, 0755)
            util.CheckErrSprintf(err, "error: failed to create override directory; %v", err)
            //os.Stdout.WriteString("info: created override directory; " + overrideDir + "\n")
        } else {
            //os.Stdout.WriteString("info: config override directory exists; " + overrideDir + "\n")
        }
		overridePath := fmt.Sprintf("%s.d/env.conf", serviceFile)
		if _, err := os.Stat(overridePath); err != nil {
			f, err := os.Create(overridePath)
            util.CheckErrSprintf(err, "error: failed to create ocerride file; %v", err)
            //os.Stdout.WriteString("info: created override file; " + overridePath + "\n")
			w := bufio.NewWriter(f)
			fmt.Fprintf(w, "# Generated by AppScope\n")
			fmt.Fprintf(w, "[Service]\n")
			fmt.Fprintf(w, "Environment=LD_PRELOAD=%s\n", libraryPath)
			fmt.Fprintf(w, "Environment=SCOPE_HOME=%s\n", configDir)
			f.Close()
        } else {
            //os.Stdout.WriteString("info: config override file exists; " + overridePath + "\n")
        }

		fmt.Printf("\nThe %s service has been updated to run with AppScope.\n", serviceName)
		fmt.Printf("\nPlease review the configs in %s/ and check their permissions to\nensure the scoped service can read them.\n", configDir)
		fmt.Printf("\nAlso, please review permissions on %s and %s to ensure\nthe scoped service can write there.\n", logDir, runDir)
		fmt.Printf("\nRestart the service with `systemctl restart %s` so the changes take effect.\n", serviceName)
	},
}

func init() {
	metricAndEventDestFlags(serviceCmd, rc)
	serviceCmd.Flags().BoolVar(&forceFlag, "force", false, "Bypass confirmation prompt")
	serviceCmd.Flags().StringVarP(&serviceUser, "user", "u", "", "Specify owner username")
	RootCmd.AddCommand(serviceCmd)
}

func confirm(s string) bool {
	reader := bufio.NewReader(os.Stdin)
	for {
		fmt.Printf("%s [y/n]: ", s)
		resp, err := reader.ReadString('\n')
		util.CheckErrSprintf(err, "error: confirm failed; %v", err)
		resp = strings.ToLower(strings.TrimSpace(resp))
		if resp == "y" || resp == "yes" {
			return true
		} else if resp == "n" || resp == "no" {
			return false
		}
	}
}
