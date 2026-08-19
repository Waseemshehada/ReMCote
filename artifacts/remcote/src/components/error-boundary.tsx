import * as React from "react"

export class ErrorBoundary extends React.Component<
  { children: React.ReactNode; resetKey?: any },
  { hasError: boolean; error?: Error }
> {
  constructor(props: { children: React.ReactNode; resetKey?: any }) {
    super(props)
    this.state = { hasError: false }
  }

  static getDerivedStateFromError(error: Error) {
    return { hasError: true, error }
  }

  componentDidUpdate(prevProps: { resetKey?: any }) {
    if (this.props.resetKey !== prevProps.resetKey && this.state.hasError) {
      this.setState({ hasError: false, error: undefined })
    }
  }

  render() {
    if (this.state.hasError) {
      return (
        <div className="min-h-[100dvh] w-full flex items-center justify-center bg-background p-6">
          <div className="max-w-md w-full space-y-4 rounded-xl border border-card-border bg-card p-6 shadow-xl">
            <h2 className="text-xl font-bold text-destructive">Application Error</h2>
            <p className="text-sm text-muted-foreground whitespace-pre-wrap break-words font-mono">
              {this.state.error?.message || "An unexpected error occurred."}
            </p>
            <button
              onClick={() => this.setState({ hasError: false, error: undefined })}
              className="inline-flex h-9 items-center justify-center rounded-md bg-primary px-4 text-sm font-medium text-primary-foreground shadow transition-colors hover:bg-primary/90 focus-visible:outline-none focus-visible:ring-1 focus-visible:ring-ring disabled:pointer-events-none disabled:opacity-50"
            >
              Try again
            </button>
          </div>
        </div>
      )
    }

    return this.props.children
  }
}
