package cmd

import (
	"fmt"
	"os"

	"github.com/criblio/scope/history"
	"github.com/criblio/scope/run"
	"github.com/criblio/scope/util"
	"github.com/spf13/cobra"
)

// sessionByID returns a session by ID, or if -1 (not set) returns last session
func sessionByID(id int) history.SessionList {
	var sessions history.SessionList
	if id == -1 {
		sessions = history.GetSessions().Last(1)
	} else {
		sessions = history.GetSessions().ID(id)
	}
	sessionCount := len(sessions)
	if sessionCount != 1 {
		util.ErrAndExit("error expected a single session, saw: %d", sessionCount)
	}
	return sessions
}

func promptClean(sl history.SessionList) {
	fmt.Print("Invalid session, likely an invalid command was scoped. Would you like to delete this session? (default: yes) [y/n] ")
	var response string
	_, err := fmt.Scanf("%s", &response)
	util.CheckErrSprintf(err, "error reading response: %v", err)
	if !(response == "n" || response == "no") {
		sl.Remove()
	}
	os.Exit(0)
}

func helpErrAndExit(cmd *cobra.Command, errText string) {
	cmd.Help()
	fmt.Printf("\nerror: %s\n", errText)
	os.Exit(1)
}

func metricAndEventDestFlags(cmd *cobra.Command, rc *run.Config) {
	cmd.Flags().StringVar(&rc.MetricsFormat, "metricformat", "ndjson", "Set format of metrics output (statsd|ndjson)")
	cmd.Flags().StringVarP(&rc.MetricsDest, "metricdest", "m", "file:///tmp/metrics.json", "Set destination for metrics (tcp://host:port, udp://host:port, or file:///path/file.json)")
	cmd.Flags().StringVarP(&rc.EventsDest, "eventdest", "e", "file:///tmp/events.json", "Set destination for events (tcp://host:port, udp://host:port, or file:///path/file.json)")
}
